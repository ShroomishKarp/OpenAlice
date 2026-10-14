/*
===========================================================================
FAKK2 / Alice static models.

A ".tik" file describes a model in text (path, surface shaders, animation
files). Static models use ".tan" files: a header, frames (bounds, scale and
offset for the compressed vertices), and per surface the triangles, texture
coordinates and per frame vertices (3 x uint16 position, 1 x uint16 normal).

Only the first frame is used for now, drawn as plain triangle surfaces.
Skeletal models (.skb / .ska) are not supported yet.
===========================================================================
*/

#include "tr_local.h"

#define TAN_IDENT   ( 'T' | ( 'A' << 8 ) | ( 'N' << 16 ) | ( ' ' << 24 ))
#define TAN_VERSION 2

#define TAN_OFS_NUMFRAMES   72
#define TAN_OFS_NUMSURFACES 80
#define TAN_OFS_FRAMES      100
#define TAN_OFS_SURFACES    104
#define TAN_FRAME_SIZE      68
#define TAN_SURFACE_HEADER  104

#define TIKI_MAX_SURFACES 64

typedef struct
{
	char surface[MAX_QPATH];
	char shader[MAX_QPATH];
} tikiSurface_t;

static int ReadInt( const byte *b )
{
	return LittleLong( *(const int *)b );
}

static float ReadFloat( const byte *b )
{
	return LittleFloat( *(const float *)b );
}

/*
** Skips a { ... } block, the opening brace is the next token.
*/
static void SkipBlock( char **text )
{
	int depth = 0;
	char *token;

	while( 1 )
	{
		token = COM_ParseExt( text, qtrue );
		if( !token[0] )
		{
			return;
		}
		if( token[0] == '{' )
		{
			depth++;
		}
		else if( token[0] == '}' )
		{
			if( --depth <= 0 )
			{
				return;
			}
		}
	}
}

/*
** Static model from a .tan file, shaders by surface name from the .tik.
*/
static qboolean R_LoadTAN( model_t *mod, const byte *buf, int len, const char *name, float scale,
			   const char *path, const tikiSurface_t *tikSurf, int numTikSurf, const char *defaultShader )
{
	int       numSurfaces, i, s, numFrames, f;
	float     totalTime;
	const byte *surf;
	srfTriangles_t **surfaces;
	shader_t **shaders;
	int       ofsFrames, ofsSurfaces;

	if( len < 176 || ReadInt( buf ) != TAN_IDENT || ReadInt( buf + 4 ) != TAN_VERSION )
	{
		ri.Printf( PRINT_WARNING, "R_LoadTAN: %s is not a TAN v%d file\n", name, TAN_VERSION );
		return qfalse;
	}

	numSurfaces = ReadInt( buf + TAN_OFS_NUMSURFACES );
	ofsFrames = ReadInt( buf + TAN_OFS_FRAMES );
	ofsSurfaces = ReadInt( buf + TAN_OFS_SURFACES );
	numFrames = ReadInt( buf + TAN_OFS_NUMFRAMES );
	totalTime = ReadFloat( buf + 84 );
	if( numFrames < 1 || numFrames > 4096 || numSurfaces < 1 || numSurfaces > TIKI_MAX_SURFACES
	    || ofsFrames + numFrames * TAN_FRAME_SIZE > len || ofsSurfaces >= len )
	{
		ri.Printf( PRINT_WARNING, "R_LoadTAN: %s has a bad header\n", name );
		return qfalse;
	}

	surfaces = ri.Hunk_Alloc( numSurfaces * numFrames * sizeof( *surfaces ), h_low );
	shaders = ri.Hunk_Alloc( numSurfaces * sizeof( *shaders ), h_low );

	surf = buf + ofsSurfaces;
	for( s = 0; s < numSurfaces; s++ )
	{
		char           surfName[MAX_QPATH];
		const char     *shaderFile = defaultShader;
		int            numVerts, numTris, ofsTris, ofsST, ofsXyz, ofsEnd;
		srfTriangles_t *tri;
		drawVert_t     *dv;
		int            *idx;
		const byte     *st, *xyz;
		char           shaderName[MAX_QPATH];

		if(( surf - buf ) + TAN_SURFACE_HEADER > len || ReadInt( surf ) != TAN_IDENT )
		{
			ri.Printf( PRINT_WARNING, "R_LoadTAN: %s has a bad surface %d\n", name, s );
			return qfalse;
		}
		Q_strncpyz( surfName, (const char *)surf + 4, sizeof( surfName ));
		numVerts = ReadInt( surf + 72 );
		numTris = ReadInt( surf + 80 );
		ofsTris = ReadInt( surf + 84 );
		ofsST = ReadInt( surf + 92 );
		ofsXyz = ReadInt( surf + 96 );
		ofsEnd = ReadInt( surf + 100 );
		if( numVerts < 1 || numTris < 1 || ( surf - buf ) + ofsEnd > len
		    || ofsXyz + numVerts * 8 > ofsEnd || ofsST + numVerts * 8 > ofsEnd || ofsTris + numTris * 12 > ofsEnd )
		{
			ri.Printf( PRINT_WARNING, "R_LoadTAN: %s has a bad surface %d\n", name, s );
			return qfalse;
		}

		// shader: by surface name, else "all", else the first one
		for( i = 0; i < numTikSurf; i++ )
		{
			if( !Q_stricmp( tikSurf[i].surface, surfName ) || !Q_stricmp( tikSurf[i].surface, "all" ))
			{
				shaderFile = tikSurf[i].shader;
				break;
			}
		}
		if( shaderFile && shaderFile[0] )
		{
			if( strchr( shaderFile, '/' ))
			{
				Q_strncpyz( shaderName, shaderFile, sizeof( shaderName ));
			}
			else
			{
				Com_sprintf( shaderName, sizeof( shaderName ), "%s/%s", path, shaderFile );
			}
			shaders[s] = R_FindShader( shaderName, LIGHTMAP_NONE, qtrue, qtrue, qtrue );
		}
		else
		{
			shaders[s] = tr.defaultShader;
		}

		idx = ri.Hunk_Alloc( numTris * 3 * sizeof( int ), h_low );
		for( i = 0; i < numTris * 3; i++ )
		{
			idx[i] = ReadInt( surf + ofsTris + i * 4 );
			if( idx[i] < 0 || idx[i] >= numVerts )
			{
				ri.Printf( PRINT_WARNING, "R_LoadTAN: %s has a bad index\n", name );
				return qfalse;
			}
		}

		st = surf + ofsST;
		for( f = 0; f < numFrames; f++ )
		{
			// frame: mins[3] maxs[3] scale[3] offset[3] delta[3] radius time
			float frameScale[3], frameOffset[3];
			int   k;

			for( k = 0; k < 3; k++ )
			{
				frameScale[k] = ReadFloat( buf + ofsFrames + f * TAN_FRAME_SIZE + 24 + k * 4 );
				frameOffset[k] = ReadFloat( buf + ofsFrames + f * TAN_FRAME_SIZE + 36 + k * 4 );
			}

			tri = ri.Hunk_Alloc( sizeof( *tri ), h_low );
			tri->surfaceType = SF_TRIANGLES;
			tri->numVerts = numVerts;
			tri->numIndexes = numTris * 3;
			tri->indexes = idx; // shared by all frames
			tri->verts = ri.Hunk_Alloc( numVerts * sizeof( drawVert_t ), h_low );

			xyz = surf + ofsXyz + f * numVerts * 8;
			ClearBounds( tri->bounds[0], tri->bounds[1] );
			for( i = 0, dv = tri->verts; i < numVerts; i++, dv++ )
			{
				const unsigned short *p = (const unsigned short *)( xyz + i * 8 );
				unsigned short       n = LittleShort( p[3] );
				float                lat = (( n >> 8 ) & 0xff ) * ( 2.0f * M_PI / 256.0f );
				float                lng = ( n & 0xff ) * ( 2.0f * M_PI / 256.0f );

				for( k = 0; k < 3; k++ )
				{
					dv->xyz[k] = (( LittleShort( p[k] ) & 0xffff ) * frameScale[k] + frameOffset[k] ) * scale;
				}
				dv->normal[0] = cos( lat ) * sin( lng );
				dv->normal[1] = sin( lat ) * sin( lng );
				dv->normal[2] = cos( lng );
				dv->st[0] = ReadFloat( st + i * 8 );
				dv->st[1] = ReadFloat( st + i * 8 + 4 );
				dv->lightmap[0] = dv->lightmap[1] = 0;
				dv->color[0] = dv->color[1] = dv->color[2] = dv->color[3] = 255;
				AddPointToBounds( dv->xyz, tri->bounds[0], tri->bounds[1] );
			}
			VectorAdd( tri->bounds[0], tri->bounds[1], tri->localOrigin );
			VectorScale( tri->localOrigin, 0.5f, tri->localOrigin );
			tri->radius = Distance( tri->bounds[0], tri->localOrigin );

			surfaces[s * numFrames + f] = tri;
		}
		surf += ofsEnd;
	}

	mod->type = MOD_TAN;
	mod->numTanSurfaces = numSurfaces;
	mod->numTanFrames = numFrames;
	mod->tanFrameTime = totalTime > 0 ? totalTime / numFrames : 1.0f / 30.0f;
	mod->tanSurfaces = surfaces;
	mod->tanShaders = shaders;
	mod->dataSize += len;
	return qtrue;
}

/*
===============
R_LoadTIKI

Loads a .tik: the static model of its first .tan animation.
===============
*/
qboolean R_LoadTIKI( model_t *mod, const char *name )
{
	char          *buf, *text, *token;
	char          path[MAX_QPATH] = "";
	char          tanFile[MAX_QPATH] = "";
	tikiSurface_t surfs[TIKI_MAX_SURFACES];
	int           numSurfs = 0, len;
	float         scale = 1.0f;
	qboolean      skeletal = qfalse;
	byte          *tan;
	qboolean      ok;

	len = ri.FS_ReadFile( name, (void **)&buf );
	if( len <= 0 || !buf )
	{
		return qfalse;
	}

	text = buf;
	token = COM_ParseExt( &text, qtrue ); // "TIKI"
	if( Q_stricmp( token, "TIKI" ))
	{
		ri.FS_FreeFile( buf );
		return qfalse;
	}

	while( 1 )
	{
		token = COM_ParseExt( &text, qtrue );
		if( !token[0] )
		{
			break;
		}

		if( !Q_stricmp( token, "setup" ))
		{
			COM_ParseExt( &text, qtrue ); // {
			while( 1 )
			{
				token = COM_ParseExt( &text, qtrue );
				if( !token[0] || token[0] == '}' )
				{
					break;
				}
				if( !Q_stricmp( token, "scale" ))
				{
					scale = atof( COM_ParseExt( &text, qfalse ));
				}
				else if( !Q_stricmp( token, "path" ))
				{
					Q_strncpyz( path, COM_ParseExt( &text, qfalse ), sizeof( path ));
				}
				else if( !Q_stricmp( token, "skelmodel" ))
				{
					skeletal = qtrue;
					SkipRestOfLine( &text );
				}
				else if( !Q_stricmp( token, "surface" ) && numSurfs < TIKI_MAX_SURFACES )
				{
					Q_strncpyz( surfs[numSurfs].surface, COM_ParseExt( &text, qfalse ), sizeof( surfs[0].surface ));
					surfs[numSurfs].shader[0] = 0;
					while( 1 )
					{
						token = COM_ParseExt( &text, qfalse );
						if( !token[0] )
						{
							break;
						}
						if( !Q_stricmp( token, "shader" ))
						{
							Q_strncpyz( surfs[numSurfs].shader, COM_ParseExt( &text, qfalse ), sizeof( surfs[0].shader ));
						}
					}
					numSurfs++;
				}
				else
				{
					SkipRestOfLine( &text );
				}
			}
		}
		else if( !Q_stricmp( token, "animations" ))
		{
			// the first animation that is a .tan file
			int depth = 0;

			while( 1 )
			{
				token = COM_ParseExt( &text, qtrue );
				if( !token[0] )
				{
					break;
				}
				if( token[0] == '{' )
				{
					depth++;
				}
				else if( token[0] == '}' )
				{
					if( --depth <= 0 )
					{
						break;
					}
				}
				else if( !tanFile[0] && strlen( token ) > 4 && !Q_stricmp( token + strlen( token ) - 4, ".tan" ))
				{
					Q_strncpyz( tanFile, token, sizeof( tanFile ));
				}
				else if( !Q_stricmp( token, "skelmodel" ))
				{
					skeletal = qtrue;
				}
			}
		}
		else if( !Q_stricmp( token, "init" ))
		{
			SkipBlock( &text );
		}
	}
	ri.FS_FreeFile( buf );

	// "path models/halo/" would make "models/halo//x.tan"
	while( path[0] && path[strlen( path ) - 1] == '/' )
	{
		path[strlen( path ) - 1] = 0;
	}

	if( skeletal || !tanFile[0] || !path[0] )
	{
		return qfalse; // skeletal (.skb/.ska) or no static animation: not supported yet
	}

	{
		char file[MAX_QPATH];

		Com_sprintf( file, sizeof( file ), "%s/%s", path, tanFile );
		len = ri.FS_ReadFile( file, (void **)&tan );
		if( len <= 0 || !tan )
		{
			ri.Printf( PRINT_DEVELOPER, "R_LoadTIKI: %s not found for %s\n", file, name );
			return qfalse;
		}
		ok = R_LoadTAN( mod, tan, len, file, scale, path, surfs, numSurfs, "" );
		ri.FS_FreeFile( tan );
	}
	return ok;
}
