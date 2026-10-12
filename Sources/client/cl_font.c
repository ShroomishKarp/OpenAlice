/*
===========================================================================
Console font support for FAKK2 / Alice data.

Quake 3 draws the console with gfx/2d/bigchars, a 16x16 grid of 8x8
characters. FAKK2 data has no such image: its text is drawn with
".ritualfont" files, a small text format that maps character codes to glyph
rectangles inside a font texture (gfx/fonts/<name>.tga).

This loads one of those fonts and lets the console draw with it when there
is no bigchars image.
===========================================================================
*/

#include "client.h"

#define MAX_FONT_GLYPHS 512

typedef struct
{
	qboolean loaded;
	qhandle_t shader;
	short    glyph[256];                // character code -> glyph index, -1 if none
	float    s0[MAX_FONT_GLYPHS], t0[MAX_FONT_GLYPHS];
	float    s1[MAX_FONT_GLYPHS], t1[MAX_FONT_GLYPHS];
	qboolean hasGlyph[MAX_FONT_GLYPHS];
} consoleFont_t;

static consoleFont_t font;

static float ParseFloat( char **text )
{
	return atof( COM_ParseExt( text, qtrue ));
}

/*
** The width and height of a TGA are in its 18 byte header, no need to
** involve the renderer.
*/
static qboolean ReadTGASize( const char *name, int *width, int *height )
{
	byte *buf;
	int  len;

	len = FS_ReadFile( name, (void **)&buf );
	if( len < 18 || !buf )
	{
		if( buf )
			FS_FreeFile( buf );
		return qfalse;
	}
	*width = buf[12] | ( buf[13] << 8 );
	*height = buf[14] | ( buf[15] << 8 );
	FS_FreeFile( buf );
	return *width > 0 && *height > 0;
}

/*
===============
CL_InitConsoleFont

name is the font name without path or extension, e.g. "courier-14".
===============
*/
void CL_InitConsoleFont( const char *name )
{
	char       path[MAX_QPATH];
	char       *buf, *text;
	const char *token;
	int        len, i, n, w, h;

	Com_Memset( &font, 0, sizeof( font ));
	for( i = 0; i < 256; i++ )
		font.glyph[i] = -1;

	Com_sprintf( path, sizeof( path ), "fonts/%s.ritualfont", name );
	len = FS_ReadFile( path, (void **)&buf );
	if( len <= 0 || !buf )
	{
		Com_Printf( "Console font %s not found\n", path );
		return;
	}

	Com_sprintf( path, sizeof( path ), "gfx/fonts/%s.tga", name );
	if( !ReadTGASize( path, &w, &h ))
	{
		Com_Printf( "Console font texture %s not found\n", path );
		FS_FreeFile( buf );
		return;
	}

	text = buf;
	token = COM_ParseExt( &text, qtrue ); // "RitFont"
	if( Q_stricmp( token, "RitFont" ))
	{
		Com_Printf( "%s is not a ritualfont\n", name );
		FS_FreeFile( buf );
		return;
	}

	while( 1 )
	{
		token = COM_ParseExt( &text, qtrue );
		if( !token[0] )
			break;

		if( !Q_stricmp( token, "indirections" ))
		{
			COM_ParseExt( &text, qtrue ); // {
			for( i = 0;; i++ )
			{
				token = COM_ParseExt( &text, qtrue );
				if( !token[0] || token[0] == '}' )
					break;
				if( i < 256 )
					font.glyph[i] = atoi( token );
			}
		}
		else if( !Q_stricmp( token, "locations" ))
		{
			COM_ParseExt( &text, qtrue ); // {
			for( n = 0; n < MAX_FONT_GLYPHS; n++ )
			{
				float x, y, gw, gh;

				token = COM_ParseExt( &text, qtrue );
				if( !token[0] || token[0] == '}' )
					break;
				// token is the "{" of one glyph
				x = ParseFloat( &text );
				y = ParseFloat( &text );
				gw = ParseFloat( &text );
				gh = ParseFloat( &text );
				COM_ParseExt( &text, qtrue ); // }

				if( gw > 0 && gh > 0 )
				{
					font.s0[n] = x / w;
					font.t0[n] = y / h;
					font.s1[n] = ( x + gw ) / w;
					font.t1[n] = ( y + gh ) / h;
					font.hasGlyph[n] = qtrue;
				}
			}
			break;
		}
	}
	FS_FreeFile( buf );

	Com_sprintf( path, sizeof( path ), "gfx/fonts/%s", name );
	font.shader = re.RegisterShaderNoMip( path );
	if( !font.shader )
	{
		Com_Printf( "Couldn't register shader %s\n", path );
		return;
	}

	font.loaded = qtrue;
	Com_Printf( "Console font: %s (%dx%d)\n", name, w, h );
}

/*
===============
CL_DrawFontChar

Draws one character with the console font, in screen pixels.
Returns qfalse if no font is loaded, so that the caller falls back to
the bigchars image. A character the font has no glyph for draws nothing.
===============
*/
qboolean CL_DrawFontChar( float x, float y, float w, float h, int ch )
{
	int g;

	if( !font.loaded )
		return qfalse;

	g = font.glyph[ch & 255];
	if( g < 0 || g >= MAX_FONT_GLYPHS || !font.hasGlyph[g] )
		return qtrue;

	re.DrawStretchPic( x, y, w, h, font.s0[g], font.t0[g], font.s1[g], font.t1[g], font.shader );
	return qtrue;
}
