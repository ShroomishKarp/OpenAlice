/*
===========================================================================
Developer map viewer.

Without a game module there is no server and no cgame, so nothing ever asks
the renderer to load a map. This gives the renderer something to chew on:

  viewmap <name>                 load maps/<name>.bsp, fly around with the
                                 movement keys and the mouse
  viewpos [x y z [yaw [pitch]]]  print or set the camera position

The camera is a plain free-fly camera, there is no collision or game logic.
===========================================================================
*/

#include "client.h"

extern kbutton_t in_forward, in_back, in_moveleft, in_moveright, in_up, in_down;

#define VIEW_FLY_SPEED   400.0f // units per second
#define VIEW_FAST_FACTOR 4.0f   // while the speed key is held

#define MAX_SKY_CAMERAS 16
#define MAX_VIEW_MODELS 2048

typedef struct
{
	qhandle_t hModel;
	vec3_t    origin;
	vec3_t    angles;
} viewModel_t;

static struct
{
	qboolean worldLoaded;   // the renderer can load only one world per init
	qboolean active;
	vec3_t   origin;
	vec3_t   angles;
	char     name[MAX_QPATH];

	// FAKK2 portal sky cameras found in the entity string (script_object
	// entities whose targetname is sky_camera<N> / skycamera<N>). The game
	// scripts pick the active one, we just let the user choose.
	int      numSkyCameras;
	int      skyCamera;     // index, -1 = no sky
	vec3_t   skyOrigin[MAX_SKY_CAMERAS];
	char     skyName[MAX_SKY_CAMERAS][32];

	// entities with a .tik model (props, pickups, ...)
	int         numModels;
	viewModel_t models[MAX_VIEW_MODELS];
} vmap;

qboolean CL_ViewMapActive( void )
{
	return vmap.active && vmap.worldLoaded;
}

/*
** called when the renderer is (re)initialized: the old world is gone
*/
void CL_ViewMapRendererRestarted( void )
{
	vmap.worldLoaded = qfalse;
	vmap.active = qfalse;
}

/*
===============
Find the sky portal cameras in the entity string.
===============
*/
static void FindSkyCameras( const char *entities )
{
	const char *text = entities;
	const char *token;

	vmap.numSkyCameras = 0;
	vmap.skyCamera = -1;

	while( text && ( token = COM_Parse( (char **)&text )) && token[0] == '{' )
	{
		char     targetname[MAX_QPATH] = "";
		vec3_t   org = { 0, 0, 0 };
		qboolean hasOrigin = qfalse;

		while( 1 )
		{
			char key[MAX_QPATH];

			token = COM_Parse( (char **)&text );
			if( !token[0] || token[0] == '}' )
			{
				break;
			}
			Q_strncpyz( key, token, sizeof( key ));
			token = COM_Parse( (char **)&text );

			if( !Q_stricmp( key, "targetname" ))
			{
				Q_strncpyz( targetname, token, sizeof( targetname ));
			}
			else if( !Q_stricmp( key, "origin" ))
			{
				if( sscanf( token, "%f %f %f", &org[0], &org[1], &org[2] ) == 3 )
				{
					hasOrigin = qtrue;
				}
			}
		}

		if( hasOrigin && vmap.numSkyCameras < MAX_SKY_CAMERAS
		    && ( !Q_stricmpn( targetname, "sky_camera", 10 ) || !Q_stricmpn( targetname, "skycamera", 9 )))
		{
			VectorCopy( org, vmap.skyOrigin[vmap.numSkyCameras] );
			Q_strncpyz( vmap.skyName[vmap.numSkyCameras], targetname, sizeof( vmap.skyName[0] ));
			vmap.numSkyCameras++;
		}
	}

	// start with the first one (entities come in map order, not numeric
	// order, so pick the lowest-sorting name)
	if( vmap.numSkyCameras )
	{
		int i, best = 0;

		for( i = 1; i < vmap.numSkyCameras; i++ )
		{
			if( Q_stricmp( vmap.skyName[i], vmap.skyName[best] ) < 0 )
			{
				best = i;
			}
		}
		vmap.skyCamera = best;
	}
}

/*
===============
Register the .tik models of the map's entities.
===============
*/
static void LoadEntityModels( const char *entities )
{
	const char *text = entities;
	const char *token;
	int        tried = 0, failed = 0;

	vmap.numModels = 0;

	while( text && ( token = COM_Parse( (char **)&text )) && token[0] == '{' )
	{
		char     model[MAX_QPATH] = "";
		vec3_t   org = { 0, 0, 0 }, ang = { 0, 0, 0 };

		while( 1 )
		{
			char key[MAX_QPATH];

			token = COM_Parse( (char **)&text );
			if( !token[0] || token[0] == '}' )
			{
				break;
			}
			Q_strncpyz( key, token, sizeof( key ));
			token = COM_Parse( (char **)&text );

			if( !Q_stricmp( key, "model" ))
			{
				Q_strncpyz( model, token, sizeof( model ));
			}
			else if( !Q_stricmp( key, "origin" ))
			{
				sscanf( token, "%f %f %f", &org[0], &org[1], &org[2] );
			}
			else if( !Q_stricmp( key, "angle" ))
			{
				ang[YAW] = atof( token );
			}
			else if( !Q_stricmp( key, "angles" ))
			{
				sscanf( token, "%f %f %f", &ang[PITCH], &ang[YAW], &ang[ROLL] );
			}
		}

		if( strlen( model ) > 4 && !Q_stricmp( model + strlen( model ) - 4, ".tik" ) && vmap.numModels < MAX_VIEW_MODELS )
		{
			qhandle_t h;

			// entities name models without the directory
			if( !strchr( model, '/' ))
			{
				char full[MAX_QPATH];

				Com_sprintf( full, sizeof( full ), "models/%s", model );
				Q_strncpyz( model, full, sizeof( model ));
			}
			h = re.RegisterModel( model );

			tried++;
			if( !h )
			{
				failed++;
				Com_DPrintf( "viewmap: no model for %s\n", model );
				continue;
			}
			vmap.models[vmap.numModels].hModel = h;
			VectorCopy( org, vmap.models[vmap.numModels].origin );
			VectorCopy( ang, vmap.models[vmap.numModels].angles );
			vmap.numModels++;
		}
	}

	Com_Printf( "%d of %d .tik models shown (the rest are skeletal, not supported yet)\n", vmap.numModels, tried );
}

/*
===============
Entity string parsing: find the first player start.
===============
*/
static qboolean FindStart( const char *entities, vec3_t origin, float *yaw )
{
	const char *text = entities;
	const char *token;
	qboolean   found = qfalse;

	while( text && ( token = COM_Parse( (char **)&text )) && token[0] == '{' )
	{
		char     classname[MAX_QPATH] = "";
		vec3_t   org = { 0, 0, 0 };
		float    ang = 0;
		qboolean hasOrigin = qfalse;

		while( 1 )
		{
			char key[MAX_QPATH];

			token = COM_Parse( (char **)&text );
			if( !token[0] || token[0] == '}' )
			{
				break;
			}
			Q_strncpyz( key, token, sizeof( key ));
			token = COM_Parse( (char **)&text );

			if( !Q_stricmp( key, "classname" ))
			{
				Q_strncpyz( classname, token, sizeof( classname ));
			}
			else if( !Q_stricmp( key, "origin" ))
			{
				if( sscanf( token, "%f %f %f", &org[0], &org[1], &org[2] ) == 3 )
				{
					hasOrigin = qtrue;
				}
			}
			else if( !Q_stricmp( key, "angle" ))
			{
				ang = atof( token );
			}
		}

		if( hasOrigin && !Q_stricmpn( classname, "info_player", 11 ))
		{
			VectorCopy( org, origin );
			*yaw = ang;
			return qtrue;
		}
		if( hasOrigin && !found && Q_stricmpn( classname, "light", 5 ))
		{
			// fallback: the first non-light entity that has a position
			VectorCopy( org, origin );
			*yaw = ang;
			found = qtrue;
		}
	}
	return found;
}

static void CL_ViewMap_f( void )
{
	char     name[MAX_QPATH];
	char     *arg;
	int      checksum;
	vec3_t   origin = { 0, 0, 0 };
	float    yaw = 0;

	if( Cmd_Argc() != 2 )
	{
		Com_Printf( "usage: viewmap <mapname>\n" );
		return;
	}
	if( vmap.worldLoaded )
	{
		Com_Printf( "A map is already loaded. Run vid_restart (or restart the engine) first.\n" );
		return;
	}

	arg = Cmd_Argv( 1 );
	if( !Q_stricmpn( arg, "maps/", 5 ))
	{
		arg += 5;
	}
	COM_StripExtension( arg, name );
	Com_sprintf( vmap.name, sizeof( vmap.name ), "maps/%s.bsp", name );

	if( FS_ReadFile( vmap.name, NULL ) <= 0 )
	{
		Com_Printf( "%s not found\n", vmap.name );
		return;
	}

	// the collision model gives us the entity string
	CM_LoadMap( vmap.name, qtrue, &checksum );
	FindSkyCameras( CM_EntityString());
	if( !FindStart( CM_EntityString(), origin, &yaw ))
	{
		Com_Printf( "no start position found in %s, starting at the origin\n", vmap.name );
	}

	re.LoadWorld( vmap.name );
	LoadEntityModels( CM_EntityString());
	re.EndRegistration();
	vmap.worldLoaded = qtrue;

	VectorCopy( origin, vmap.origin );
	vmap.origin[2] += 40; // eye height
	VectorSet( vmap.angles, 0, yaw, 0 );
	vmap.active = qtrue;

	// leave the console, the view is what we want to see now
	cls.keyCatchers &= ~KEYCATCH_CONSOLE;
	Con_Close();

	// movement keys, only where the user has nothing bound. (Alice's own
	// default.cfg binds w/a/s/d, space = up, f = down and shift = speed.)
	if( !Key_GetBinding( 'w' )[0] ) Key_SetBinding( 'w', "+forward" );
	if( !Key_GetBinding( 's' )[0] ) Key_SetBinding( 's', "+back" );
	if( !Key_GetBinding( 'a' )[0] ) Key_SetBinding( 'a', "+moveleft" );
	if( !Key_GetBinding( 'd' )[0] ) Key_SetBinding( 'd', "+moveright" );
	if( !Key_GetBinding( K_SPACE )[0] ) Key_SetBinding( K_SPACE, "+moveup" );
	if( !Key_GetBinding( 'f' )[0] ) Key_SetBinding( 'f', "+movedown" );
	if( !Key_GetBinding( K_SHIFT )[0] ) Key_SetBinding( K_SHIFT, "+speed" );

	Com_Printf( "viewing %s at %.0f %.0f %.0f (WASD, space/f = up/down, shift = fast, ~ = console)\n",
		    vmap.name, vmap.origin[0], vmap.origin[1], vmap.origin[2] );
	if( vmap.numSkyCameras )
	{
		Com_Printf( "%d sky camera(s), using %s (see skycam)\n", vmap.numSkyCameras, vmap.skyName[vmap.skyCamera] );
	}
}

static void CL_ViewPos_f( void )
{
	if( Cmd_Argc() == 1 )
	{
		Com_Printf( "viewpos %.1f %.1f %.1f %.1f %.1f\n",
			    vmap.origin[0], vmap.origin[1], vmap.origin[2], vmap.angles[YAW], vmap.angles[PITCH] );
		return;
	}
	if( Cmd_Argc() < 4 )
	{
		Com_Printf( "usage: viewpos [x y z [yaw [pitch]]]\n" );
		return;
	}
	vmap.origin[0] = atof( Cmd_Argv( 1 ));
	vmap.origin[1] = atof( Cmd_Argv( 2 ));
	vmap.origin[2] = atof( Cmd_Argv( 3 ));
	if( Cmd_Argc() > 4 )
	{
		vmap.angles[YAW] = atof( Cmd_Argv( 4 ));
	}
	if( Cmd_Argc() > 5 )
	{
		vmap.angles[PITCH] = atof( Cmd_Argv( 5 ));
	}
}

/*
** skycam            list the sky cameras of the map
** skycam <n>        use camera n
** skycam off        no sky portal
*/
static void CL_SkyCam_f( void )
{
	int i;

	if( Cmd_Argc() == 1 )
	{
		for( i = 0; i < vmap.numSkyCameras; i++ )
		{
			Com_Printf( "%s%d: %s at %.0f %.0f %.0f\n", i == vmap.skyCamera ? "* " : "  ", i,
				    vmap.skyName[i], vmap.skyOrigin[i][0], vmap.skyOrigin[i][1], vmap.skyOrigin[i][2] );
		}
		if( !vmap.numSkyCameras )
		{
			Com_Printf( "no sky cameras in this map\n" );
		}
		return;
	}

	if( !Q_stricmp( Cmd_Argv( 1 ), "off" ))
	{
		vmap.skyCamera = -1;
		return;
	}

	i = atoi( Cmd_Argv( 1 ));
	if( i < 0 || i >= vmap.numSkyCameras )
	{
		Com_Printf( "no sky camera %d\n", i );
		return;
	}
	vmap.skyCamera = i;
}

void CL_InitViewMap( void )
{
	Cmd_AddCommand( "viewmap", CL_ViewMap_f );
	Cmd_AddCommand( "viewpos", CL_ViewPos_f );
	Cmd_AddCommand( "skycam", CL_SkyCam_f );
}

/*
===============
CL_ViewMapMouse

Returns qtrue if the mouse movement was used for looking around.
===============
*/
qboolean CL_ViewMapMouse( int dx, int dy )
{
	float scale;

	if( !CL_ViewMapActive() || ( cls.keyCatchers & ( KEYCATCH_CONSOLE | KEYCATCH_UI | KEYCATCH_CGAME )))
	{
		return qfalse;
	}

	scale = 0.022f * cl_sensitivity->value;
	vmap.angles[YAW] -= dx * scale;
	vmap.angles[PITCH] += dy * scale;
	if( vmap.angles[PITCH] > 89 )
		vmap.angles[PITCH] = 89;
	if( vmap.angles[PITCH] < -89 )
		vmap.angles[PITCH] = -89;
	return qtrue;
}

/*
===============
CL_ViewMapFrame
===============
*/
void CL_ViewMapFrame( int msec )
{
	vec3_t forward, right, up, move;
	float  speed, f, r, u;

	if( !CL_ViewMapActive() || ( cls.keyCatchers & ( KEYCATCH_CONSOLE | KEYCATCH_UI | KEYCATCH_CGAME )))
	{
		return;
	}

	// Not CL_KeyState(): it divides by frame_msec, which only CL_CreateCmd()
	// sets, and that does not run while we are not connected (0/0 = NaN).
	f = ( in_forward.active ? 1.0f : 0.0f ) - ( in_back.active ? 1.0f : 0.0f );
	r = ( in_moveright.active ? 1.0f : 0.0f ) - ( in_moveleft.active ? 1.0f : 0.0f );
	u = ( in_up.active ? 1.0f : 0.0f ) - ( in_down.active ? 1.0f : 0.0f );

	speed = VIEW_FLY_SPEED * ( msec / 1000.0f );
	if( in_speed.active )
	{
		speed *= VIEW_FAST_FACTOR;
	}

	AngleVectors( vmap.angles, forward, right, up );
	VectorScale( forward, f * speed, move );
	VectorMA( move, r * speed, right, move );
	move[2] += u * speed;
	VectorAdd( vmap.origin, move, vmap.origin );
}

/*
===============
CL_ViewMapDraw
===============
*/
void CL_ViewMapDraw( void )
{
	refdef_t rd;
	float    x;
	int      i;

	if( !CL_ViewMapActive() )
	{
		return;
	}

	Com_Memset( &rd, 0, sizeof( rd ));
	rd.x = 0;
	rd.y = 0;
	rd.width = cls.glconfig.vidWidth;
	rd.height = cls.glconfig.vidHeight;

	rd.fov_x = 90;
	x = rd.width / tan( rd.fov_x / 360 * M_PI );
	rd.fov_y = atan2( rd.height, x ) * 360 / M_PI;

	VectorCopy( vmap.origin, rd.vieworg );
	AnglesToAxis( vmap.angles, rd.viewaxis );
	rd.time = cls.realtime;
	rd.rdflags = 0;

	if( vmap.skyCamera >= 0 )
	{
		rd.hasSkyPortal = qtrue;
		VectorCopy( vmap.skyOrigin[vmap.skyCamera], rd.skyPortalOrigin );
	}

	re.ClearScene();
	for( i = 0; i < vmap.numModels; i++ )
	{
		refEntity_t ent;

		Com_Memset( &ent, 0, sizeof( ent ));
		ent.reType = RT_MODEL;
		ent.hModel = vmap.models[i].hModel;
		VectorCopy( vmap.models[i].origin, ent.origin );
		VectorCopy( vmap.models[i].origin, ent.lightingOrigin );
		AnglesToAxis( vmap.models[i].angles, ent.axis );
		ent.renderfx = RF_LIGHTING_ORIGIN;
		re.AddRefEntityToScene( &ent );
	}
	re.RenderScene( &rd );
}
