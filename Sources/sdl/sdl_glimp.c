/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.
===========================================================================
*/
/*
** SDL_GLIMP.C
**
** SDL3 implementation of the platform layer that used to live in
** unix/linux_glimp.c (X11 + GLX + XF86VidMode + DGA):
**
**   window and OpenGL context   GLimp_Init / GLimp_Shutdown / GLimp_EndFrame
**   keyboard, mouse, text       IN_Init / IN_Frame / Sys_SendKeyEvents
**
** Sound lives in sdl_snd.c.
*/

#include <SDL3/SDL.h>
#include <string.h>
#include <stdio.h>

#include "../renderer/tr_local.h"
#include "../client/client.h"
#include "../unix/linux_local.h"
#include "../unix/unix_glw.h"

glwstate_t glw_state;

typedef enum
{
	RSERR_OK,
	RSERR_INVALID_FULLSCREEN,
	RSERR_INVALID_MODE,
	RSERR_UNKNOWN
} rserr_t;

static SDL_Window    *sdl_window = NULL;
static SDL_GLContext sdl_context = NULL;

static cvar_t        *in_mouse;
static cvar_t        *in_nograb; // developer feature: never grab the mouse
static cvar_t        *r_allowSoftwareGL;

static qboolean      mouse_avail;
static qboolean      mouse_active;
static qboolean      video_inited;

/*
=============================================================================

KEYBOARD

=============================================================================
*/

static const char *Q_stristr( const char *s, const char *find )
{
	char   c, sc;
	size_t len;

	if(( c = *find++ ) != 0 )
	{
		if( c >= 'a' && c <= 'z' )
			c -= ( 'a' - 'A' );
		len = strlen( find );
		do
		{
			do
			{
				if(( sc = *s++ ) == 0 )
					return NULL;
				if( sc >= 'a' && sc <= 'z' )
					sc -= ( 'a' - 'A' );
			}
			while( sc != c );
		}
		while( Q_stricmpn( s, find, len ) != 0 );
		s--;
	}
	return s;
}

/*
** Translate an SDL key event into an engine key code (see ui/keycodes.h).
** Returns 0 for keys the engine has no code for.
*/
static int TranslateKey( const SDL_KeyboardEvent *ev )
{
	SDL_Keycode sym = ev->key;

	// the console key is a physical key: keep it working on any layout
	if( ev->scancode == SDL_SCANCODE_GRAVE )
		return '`';

	// printable ASCII: SDL keycodes are already the lower case character
	if( sym >= 32 && sym < 127 )
		return (int)sym;

	switch( sym )
	{
	case SDLK_PAGEUP: return K_PGUP;
	case SDLK_KP_9: return K_KP_PGUP;
	case SDLK_PAGEDOWN: return K_PGDN;
	case SDLK_KP_3: return K_KP_PGDN;
	case SDLK_KP_7: return K_KP_HOME;
	case SDLK_HOME: return K_HOME;
	case SDLK_KP_1: return K_KP_END;
	case SDLK_END: return K_END;
	case SDLK_KP_4: return K_KP_LEFTARROW;
	case SDLK_LEFT: return K_LEFTARROW;
	case SDLK_KP_6: return K_KP_RIGHTARROW;
	case SDLK_RIGHT: return K_RIGHTARROW;
	case SDLK_KP_2: return K_KP_DOWNARROW;
	case SDLK_DOWN: return K_DOWNARROW;
	case SDLK_KP_8: return K_KP_UPARROW;
	case SDLK_UP: return K_UPARROW;
	case SDLK_ESCAPE: return K_ESCAPE;
	case SDLK_KP_ENTER: return K_KP_ENTER;
	case SDLK_RETURN: return K_ENTER;
	case SDLK_TAB: return K_TAB;
	case SDLK_F1: return K_F1;
	case SDLK_F2: return K_F2;
	case SDLK_F3: return K_F3;
	case SDLK_F4: return K_F4;
	case SDLK_F5: return K_F5;
	case SDLK_F6: return K_F6;
	case SDLK_F7: return K_F7;
	case SDLK_F8: return K_F8;
	case SDLK_F9: return K_F9;
	case SDLK_F10: return K_F10;
	case SDLK_F11: return K_F11;
	case SDLK_F12: return K_F12;
	case SDLK_BACKSPACE: return K_BACKSPACE;
	case SDLK_KP_PERIOD: return K_KP_DEL;
	case SDLK_DELETE: return K_DEL;
	case SDLK_PAUSE: return K_PAUSE;
	case SDLK_LSHIFT:
	case SDLK_RSHIFT: return K_SHIFT;
	case SDLK_LCTRL:
	case SDLK_RCTRL: return K_CTRL;
	case SDLK_LALT:
	case SDLK_RALT:
	case SDLK_LGUI:
	case SDLK_RGUI: return K_ALT;
	case SDLK_KP_5: return K_KP_5;
	case SDLK_INSERT: return K_INS;
	case SDLK_KP_0: return K_KP_INS;
	case SDLK_KP_MULTIPLY: return K_KP_STAR;
	case SDLK_KP_PLUS: return K_KP_PLUS;
	case SDLK_KP_MINUS: return K_KP_MINUS;
	case SDLK_KP_DIVIDE: return K_KP_SLASH;
	case SDLK_KP_EQUALS: return K_KP_EQUALS;
	case SDLK_NUMLOCKCLEAR: return K_KP_NUMLOCK;
	case SDLK_CAPSLOCK: return K_CAPSLOCK;
	default: break;
	}
	return 0;
}

static int mouse_button_key( Uint8 button )
{
	switch( button )
	{
	case SDL_BUTTON_LEFT: return K_MOUSE1;
	case SDL_BUTTON_MIDDLE: return K_MOUSE3;
	case SDL_BUTTON_RIGHT: return K_MOUSE2;
	case SDL_BUTTON_X1: return K_MOUSE4;
	case SDL_BUTTON_X2: return K_MOUSE5;
	}
	return 0;
}

static void KeyEvent( const SDL_KeyboardEvent *ev )
{
	int key, t;

	if( ev->repeat )
		return;

	key = TranslateKey( ev );
	if( !key )
		return;

	t = Sys_Milliseconds();
	Sys_QueEvent( t, SE_KEY, key, ev->down ? qtrue : qfalse, 0, NULL );

	if( !ev->down )
		return;

	// Typed text arrives through SDL_EVENT_TEXT_INPUT, but the console and
	// message fields also expect the control characters a terminal would
	// produce (Enter, Backspace, Tab, Ctrl+letter shortcuts).
	if( key == K_ENTER || key == K_KP_ENTER )
		Sys_QueEvent( t, SE_CHAR, '\r', 0, 0, NULL );
	else if( key == K_BACKSPACE )
		Sys_QueEvent( t, SE_CHAR, 8, 0, 0, NULL );
	else if( key == K_TAB )
		Sys_QueEvent( t, SE_CHAR, '\t', 0, 0, NULL );
	else if(( ev->mod & SDL_KMOD_CTRL ) && key >= 'a' && key <= 'z' )
		Sys_QueEvent( t, SE_CHAR, key - 'a' + 1, 0, 0, NULL );
}

static void TextEvent( const char *text )
{
	const unsigned char *s = (const unsigned char *)text;
	int                 t = Sys_Milliseconds();

	// the engine only has 8 bit characters: pass ASCII, drop the rest
	for( ; *s; s++ )
	{
		if( *s >= 32 && *s < 127 )
			Sys_QueEvent( t, SE_CHAR, *s, 0, 0, NULL );
	}
}

/*
=============================================================================

MOUSE

=============================================================================
*/

void IN_ActivateMouse( void )
{
	if( !mouse_avail || !sdl_window || mouse_active )
		return;
	if( !in_nograb->value )
		SDL_SetWindowRelativeMouseMode( sdl_window, true );
	mouse_active = qtrue;
}

void IN_DeactivateMouse( void )
{
	if( !mouse_avail || !sdl_window || !mouse_active )
		return;
	if( !in_nograb->value )
		SDL_SetWindowRelativeMouseMode( sdl_window, false );
	mouse_active = qfalse;
}

/*
=============================================================================

EVENT PUMP

=============================================================================
*/

static void HandleEvents( void )
{
	SDL_Event e;

	while( SDL_PollEvent( &e ))
	{
		switch( e.type )
		{
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP:
			KeyEvent( &e.key );
			break;

		case SDL_EVENT_TEXT_INPUT:
			TextEvent( e.text.text );
			break;

		case SDL_EVENT_MOUSE_MOTION:
			if( mouse_active )
			{
				int dx = (int)SDL_roundf( e.motion.xrel );
				int dy = (int)SDL_roundf( e.motion.yrel );
				if( dx || dy )
					Sys_QueEvent( Sys_Milliseconds(), SE_MOUSE, dx, dy, 0, NULL );
			}
			break;

		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
		{
			int key = mouse_button_key( e.button.button );
			if( key )
				Sys_QueEvent( Sys_Milliseconds(), SE_KEY, key, e.button.down ? qtrue : qfalse, 0, NULL );
			break;
		}

		case SDL_EVENT_MOUSE_WHEEL:
		{
			int t = Sys_Milliseconds();
			int key = e.wheel.y > 0 ? K_MWHEELUP : K_MWHEELDOWN;
			if( e.wheel.y != 0 )
			{
				Sys_QueEvent( t, SE_KEY, key, qtrue, 0, NULL );
				Sys_QueEvent( t, SE_KEY, key, qfalse, 0, NULL );
			}
			break;
		}

		case SDL_EVENT_WINDOW_FOCUS_LOST:
			// don't leave keys stuck down after alt-tab
			Key_ClearStates();
			break;

		case SDL_EVENT_QUIT:
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			Cbuf_ExecuteText( EXEC_APPEND, "quit\n" );
			break;

		default:
			break;
		}
	}
}

void Sys_SendKeyEvents( void )
{
	if( !video_inited )
		return;
	HandleEvents();
}

void IN_Init( void )
{
	Com_Printf( "\n------- Input Initialization -------\n" );
	in_mouse = Cvar_Get( "in_mouse", "1", CVAR_ARCHIVE );
	in_nograb = Cvar_Get( "in_nograb", "0", 0 );

	mouse_avail = in_mouse->value ? qtrue : qfalse;
	mouse_active = qfalse;
	Com_Printf( "------------------------------------\n" );
}

void IN_Shutdown( void )
{
	IN_DeactivateMouse();
	mouse_avail = qfalse;
}

void IN_Frame( void )
{
	if( !sdl_window )
		return;

	if( cls.keyCatchers & KEYCATCH_CONSOLE )
	{
		// temporarily release the mouse when the console is down and we
		// are running in a window on the desktop
		if( Cvar_VariableValue( "r_fullscreen" ) == 0 )
		{
			IN_DeactivateMouse();
			return;
		}
	}

	IN_ActivateMouse();
}

void IN_Activate( void )
{
}

// joysticks / gamepads: not implemented yet (the old Linux joystick API
// code in unix/linux_joystick.c is no longer built)
void IN_StartupJoystick( void )
{
}

void IN_JoyMove( void )
{
}

/*
=============================================================================

WINDOW AND OPENGL CONTEXT

=============================================================================
*/

void GLimp_SetGamma( unsigned char red[256], unsigned char green[256], unsigned char blue[256] )
{
	// SDL3 has no gamma ramp API. glConfig.deviceSupportsGamma is false,
	// so the renderer applies gamma to the textures itself.
}

void GLimp_LogComment( char *comment )
{
	if( glw_state.log_fp )
		fprintf( glw_state.log_fp, "%s", comment );
}

void GLimp_Shutdown( void )
{
	IN_DeactivateMouse();

	if( sdl_context )
	{
		SDL_GL_DestroyContext( sdl_context );
		sdl_context = NULL;
	}
	if( sdl_window )
	{
		SDL_StopTextInput( sdl_window );
		SDL_DestroyWindow( sdl_window );
		sdl_window = NULL;
	}
	if( video_inited )
	{
		SDL_QuitSubSystem( SDL_INIT_VIDEO );
		video_inited = qfalse;
	}

	memset( &glConfig, 0, sizeof( glConfig ));
	memset( &glState, 0, sizeof( glState ));

	QGL_Shutdown();
}

static int GLW_SetMode( int mode, qboolean fullscreen )
{
	int  colorbits, depthbits, stencilbits;
	int  tcolorbits, tdepthbits, tstencilbits;
	int  i;
	SDL_WindowFlags flags;
	const char *glstring;

	ri.Printf( PRINT_ALL, "Initializing OpenGL display\n" );
	ri.Printf( PRINT_ALL, "...setting mode %d:", mode );

	if( !R_GetModeInfo( &glConfig.vidWidth, &glConfig.vidHeight, &glConfig.windowAspect, mode ))
	{
		ri.Printf( PRINT_ALL, " invalid mode\n" );
		return RSERR_INVALID_MODE;
	}
	ri.Printf( PRINT_ALL, " %d %d\n", glConfig.vidWidth, glConfig.vidHeight );

	colorbits = r_colorbits->value;
	if( colorbits == 0 || colorbits > 24 )
		colorbits = 24;
	depthbits = r_depthbits->value;
	if( depthbits == 0 )
		depthbits = 24;
	stencilbits = r_stencilbits->value;

	// try the requested depth first, then fall back like the old code did:
	// i == 0: as requested, i == 1: relaxed depth, i == 2: relaxed stencil
	for( i = 0; i < 3; i++ )
	{
		tcolorbits = colorbits;
		tdepthbits = depthbits;
		tstencilbits = stencilbits;

		if( i == 1 )
			tdepthbits = depthbits >= 24 ? 16 : 24;
		else if( i == 2 )
			tstencilbits = 0;

		SDL_GL_ResetAttributes();
		SDL_GL_SetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY );
		SDL_GL_SetAttribute( SDL_GL_RED_SIZE, tcolorbits == 24 ? 8 : 4 );
		SDL_GL_SetAttribute( SDL_GL_GREEN_SIZE, tcolorbits == 24 ? 8 : 4 );
		SDL_GL_SetAttribute( SDL_GL_BLUE_SIZE, tcolorbits == 24 ? 8 : 4 );
		SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, tdepthbits );
		SDL_GL_SetAttribute( SDL_GL_STENCIL_SIZE, tstencilbits );
		SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );

		flags = SDL_WINDOW_OPENGL;
		if( fullscreen )
			flags |= SDL_WINDOW_FULLSCREEN;

		sdl_window = SDL_CreateWindow( "OpenAlice", glConfig.vidWidth, glConfig.vidHeight, flags );
		if( !sdl_window )
		{
			ri.Printf( PRINT_ALL, "SDL_CreateWindow failed: %s\n", SDL_GetError());
			continue;
		}

		sdl_context = SDL_GL_CreateContext( sdl_window );
		if( !sdl_context )
		{
			ri.Printf( PRINT_ALL, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
			SDL_DestroyWindow( sdl_window );
			sdl_window = NULL;
			continue;
		}

		glConfig.colorBits = tcolorbits;
		glConfig.depthBits = tdepthbits;
		glConfig.stencilBits = tstencilbits;
		break;
	}

	if( !sdl_window )
	{
		ri.Printf( PRINT_ALL, "Couldn't get a window and an OpenGL context\n" );
		return RSERR_INVALID_MODE;
	}

	SDL_GL_MakeCurrent( sdl_window, sdl_context );
	SDL_GL_SetSwapInterval( r_swapInterval->integer ? 1 : 0 );

	// the window may not have the size we asked for (tiling WMs, HiDPI)
	{
		int w, h;
		SDL_GetWindowSizeInPixels( sdl_window, &w, &h );
		if( w > 0 && h > 0 && ( w != glConfig.vidWidth || h != glConfig.vidHeight ))
		{
			ri.Printf( PRINT_ALL, "...window is %dx%d, not %dx%d\n", w, h, glConfig.vidWidth, glConfig.vidHeight );
			glConfig.vidWidth = w;
			glConfig.vidHeight = h;
		}
	}

	glConfig.isFullscreen = fullscreen;
	glConfig.stereoEnabled = qfalse;
	glConfig.deviceSupportsGamma = qfalse;

	{
		const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode( SDL_GetDisplayForWindow( sdl_window ));
		glConfig.displayFrequency = dm ? (int)( dm->refresh_rate + 0.5f ) : 0;
	}

	SDL_StartTextInput( sdl_window );

	// bind the qgl* function pointers now that a context is current
	if( !QGL_Init( NULL ))
	{
		ri.Printf( PRINT_ALL, "...couldn't bind OpenGL functions\n" );
		GLimp_Shutdown();
		return RSERR_INVALID_MODE;
	}

	glstring = (const char *)qglGetString( GL_RENDERER );
	ri.Printf( PRINT_ALL, "GL_RENDERER: %s\n", glstring ? glstring : "(null)" );

	if( glstring && ( !Q_stricmp( glstring, "Mesa X11" ) || Q_stristr( glstring, "llvmpipe" ) || Q_stristr( glstring, "softpipe" )))
	{
		if( !r_allowSoftwareGL->integer )
		{
			ri.Printf( PRINT_ALL, "\n***********************************************************\n" );
			ri.Printf( PRINT_ALL, " You are using software Mesa (no hardware acceleration)!\n" );
			ri.Printf( PRINT_ALL, " If this is intentional, add\n" );
			ri.Printf( PRINT_ALL, "       \"+set r_allowSoftwareGL 1\"\n" );
			ri.Printf( PRINT_ALL, " to the command line when starting the game.\n" );
			ri.Printf( PRINT_ALL, "***********************************************************\n" );
			GLimp_Shutdown();
			return RSERR_INVALID_MODE;
		}
		ri.Printf( PRINT_ALL, "...using software Mesa (r_allowSoftwareGL==1).\n" );
	}

	return RSERR_OK;
}

static qboolean GLW_StartDriverAndSetMode( int mode, qboolean fullscreen )
{
	int err = GLW_SetMode( mode, fullscreen );

	switch( err )
	{
	case RSERR_INVALID_FULLSCREEN:
		ri.Printf( PRINT_ALL, "...WARNING: fullscreen unavailable in this mode\n" );
		return qfalse;
	case RSERR_INVALID_MODE:
		ri.Printf( PRINT_ALL, "...WARNING: could not set the given mode (%d)\n", mode );
		return qfalse;
	default:
		break;
	}
	return qtrue;
}

static void GLW_InitExtensions( void )
{
	if( !r_allowExtensions->integer )
	{
		ri.Printf( PRINT_ALL, "*** IGNORING OPENGL EXTENSIONS ***\n" );
		return;
	}

	ri.Printf( PRINT_ALL, "Initializing OpenGL extensions\n" );

	// GL_S3_s3tc
	glConfig.textureCompression = TC_NONE;
	if( Q_stristr( glConfig.extensions_string, "GL_S3_s3tc" ))
	{
		if( r_ext_compressed_textures->value )
		{
			glConfig.textureCompression = TC_S3TC;
			ri.Printf( PRINT_ALL, "...using GL_S3_s3tc\n" );
		}
		else
			ri.Printf( PRINT_ALL, "...ignoring GL_S3_s3tc\n" );
	}
	else
		ri.Printf( PRINT_ALL, "...GL_S3_s3tc not found\n" );

	// GL_EXT_texture_env_add
	glConfig.textureEnvAddAvailable = qfalse;
	if( Q_stristr( glConfig.extensions_string, "EXT_texture_env_add" ))
	{
		if( r_ext_texture_env_add->integer )
		{
			glConfig.textureEnvAddAvailable = qtrue;
			ri.Printf( PRINT_ALL, "...using GL_EXT_texture_env_add\n" );
		}
		else
			ri.Printf( PRINT_ALL, "...ignoring GL_EXT_texture_env_add\n" );
	}
	else
		ri.Printf( PRINT_ALL, "...GL_EXT_texture_env_add not found\n" );

	// GL_ARB_multitexture
	qglMultiTexCoord2fARB = NULL;
	qglActiveTextureARB = NULL;
	qglClientActiveTextureARB = NULL;
	if( Q_stristr( glConfig.extensions_string, "GL_ARB_multitexture" ))
	{
		if( r_ext_multitexture->value )
		{
			qglMultiTexCoord2fARB = (PFNGLMULTITEXCOORD2FARBPROC)SDL_GL_GetProcAddress( "glMultiTexCoord2fARB" );
			qglActiveTextureARB = (PFNGLACTIVETEXTUREARBPROC)SDL_GL_GetProcAddress( "glActiveTextureARB" );
			qglClientActiveTextureARB = (PFNGLCLIENTACTIVETEXTUREARBPROC)SDL_GL_GetProcAddress( "glClientActiveTextureARB" );

			if( qglActiveTextureARB )
			{
				qglGetIntegerv( GL_MAX_ACTIVE_TEXTURES_ARB, &glConfig.maxActiveTextures );

				if( glConfig.maxActiveTextures > 1 )
					ri.Printf( PRINT_ALL, "...using GL_ARB_multitexture\n" );
				else
				{
					qglMultiTexCoord2fARB = NULL;
					qglActiveTextureARB = NULL;
					qglClientActiveTextureARB = NULL;
					ri.Printf( PRINT_ALL, "...not using GL_ARB_multitexture, < 2 texture units\n" );
				}
			}
		}
		else
			ri.Printf( PRINT_ALL, "...ignoring GL_ARB_multitexture\n" );
	}
	else
		ri.Printf( PRINT_ALL, "...GL_ARB_multitexture not found\n" );

	// GL_EXT_compiled_vertex_array
	if( Q_stristr( glConfig.extensions_string, "GL_EXT_compiled_vertex_array" ))
	{
		if( r_ext_compiled_vertex_array->value )
		{
			ri.Printf( PRINT_ALL, "...using GL_EXT_compiled_vertex_array\n" );
			qglLockArraysEXT = ( void( APIENTRY * )( int, int ))SDL_GL_GetProcAddress( "glLockArraysEXT" );
			qglUnlockArraysEXT = ( void( APIENTRY * )( void ))SDL_GL_GetProcAddress( "glUnlockArraysEXT" );
			if( !qglLockArraysEXT || !qglUnlockArraysEXT )
				ri.Error( ERR_FATAL, "bad getprocaddress" );
		}
		else
			ri.Printf( PRINT_ALL, "...ignoring GL_EXT_compiled_vertex_array\n" );
	}
	else
		ri.Printf( PRINT_ALL, "...GL_EXT_compiled_vertex_array not found\n" );
}

void GLimp_Init( void )
{
	char   buf[1024];
	cvar_t *lastValidRenderer = ri.Cvar_Get( "r_lastValidRenderer", "(uninitialized)", CVAR_ARCHIVE );

	r_allowSoftwareGL = ri.Cvar_Get( "r_allowSoftwareGL", "0", CVAR_LATCH );

	if( !SDL_InitSubSystem( SDL_INIT_VIDEO ))
		ri.Error( ERR_FATAL, "GLimp_Init() - SDL_InitSubSystem(VIDEO) failed: %s\n", SDL_GetError());
	video_inited = qtrue;

	// create the window and context; fall back to safer settings on failure
	if( !GLW_StartDriverAndSetMode( r_mode->integer, r_fullscreen->integer ))
	{
		// try windowed
		if( r_fullscreen->integer )
		{
			ri.Cvar_Set( "r_fullscreen", "0" );
			r_fullscreen->modified = qfalse;
			if( GLW_StartDriverAndSetMode( r_mode->integer, qfalse ))
				goto got_mode;
		}
		// try the safe mode
		if( r_mode->integer != 3 )
		{
			ri.Cvar_Set( "r_mode", "3" );
			r_mode->modified = qfalse;
			if( GLW_StartDriverAndSetMode( 3, qfalse ))
				goto got_mode;
		}
		ri.Error( ERR_FATAL, "GLimp_Init() - could not load OpenGL subsystem\n" );
	}

got_mode:
	glConfig.driverType = GLDRV_ICD;
	glConfig.hardwareType = GLHW_GENERIC;

	// get our config strings
	Q_strncpyz( glConfig.vendor_string, (const char *)qglGetString( GL_VENDOR ), sizeof( glConfig.vendor_string ));
	Q_strncpyz( glConfig.renderer_string, (const char *)qglGetString( GL_RENDERER ), sizeof( glConfig.renderer_string ));
	if( *glConfig.renderer_string && glConfig.renderer_string[strlen( glConfig.renderer_string ) - 1] == '\n' )
		glConfig.renderer_string[strlen( glConfig.renderer_string ) - 1] = 0;
	Q_strncpyz( glConfig.version_string, (const char *)qglGetString( GL_VERSION ), sizeof( glConfig.version_string ));
	Q_strncpyz( glConfig.extensions_string, (const char *)qglGetString( GL_EXTENSIONS ), sizeof( glConfig.extensions_string ));

	Q_strncpyz( buf, glConfig.renderer_string, sizeof( buf ));
	strlwr( buf );

	// first run on this hardware: pick sane defaults
	if( Q_stricmp( lastValidRenderer->string, glConfig.renderer_string ))
	{
		glConfig.hardwareType = GLHW_GENERIC;
		ri.Cvar_Set( "r_textureMode", "GL_LINEAR_MIPMAP_NEAREST" );
		ri.Cvar_Set( "r_picmip", "1" );
	}

	ri.Cvar_Set( "r_lastValidRenderer", glConfig.renderer_string );

	GLW_InitExtensions();
}

/*
** GLimp_EndFrame
**
** Responsible for doing a swapbuffers.
*/
void GLimp_EndFrame( void )
{
	// don't flip if drawing to front buffer
	if( Q_stricmp( r_drawBuffer->string, "GL_FRONT" ) != 0 )
		SDL_GL_SwapWindow( sdl_window );

	// check logging
	QGL_EnableLogging( (qboolean)r_logFile->integer );
}

/*
===========================================================

SMP acceleration

Not supported with SDL: the renderer runs on the main thread.
(GLimp_SpawnRenderThread returning qfalse makes the renderer fall
back to single threaded rendering.)

===========================================================
*/

qboolean GLimp_SpawnRenderThread( void ( *function )( void ))
{
	return qfalse;
}

void *GLimp_RendererSleep( void )
{
	return NULL;
}

void GLimp_FrontEndSleep( void )
{
}

void GLimp_WakeRenderer( void *data )
{
}
