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
==============================================================================

Skeletal models: .skb (skeleton, surfaces with weighted vertices) and .ska
(animation: per frame, per bone a compressed local rotation and offset).
Skinned in software every frame.

==============================================================================
*/

#define SKEL_IDENT      ( 'S' | ( 'K' << 8 ) | ( 'L' << 16 ) | ( ' ' << 24 ))
#define SKEL_ANIM_IDENT ( 'S' | ( 'K' << 8 ) | ( 'A' << 16 ) | ( 'N' << 24 ))
#define SKEL_VERSION    3
#define SKEL_MAXBONES   100

#define QUAT_RECIP   ( 1.0f / (( 1 << 15 ) - 1 ))
#define OFFSET_RECIP ( 1.0f / (( 1 << ( 15 - 9 )) - 1 ))

typedef struct
{
	int    bone;
	float  weight;
	vec3_t offset;
} tikiSkinWeight_t;

typedef struct
{
	vec3_t       normal;
	float        st[2];
	int          numWeights;
	tikiSkinWeight_t *weights;
} tikiSkinVert_t;

typedef struct
{
	int        numVerts, numTris;
	tikiSkinVert_t *verts;
	int        *indexes;
} tikiSkinSurface_t;

typedef struct skelModel_s
{
	int           numBones;
	int           parent[SKEL_MAXBONES];
	int           numSurfaces;
	tikiSkinSurface_t *surfaces;
	int           numFrames;
	float         frameTime;
	float         scale;
	float         *quat;   // [frame][bone][4] x y z w
	float         *offset; // [frame][bone][3]
} skelModel_t;

static qboolean R_LoadSkel( model_t *mod, const byte *skb, int skbLen, const byte *ska, int skaLen, const char *name,
			    float scale, const char *path, const tikiSurface_t *tikSurf, int numTikSurf )
{
	skelModel_t *sk;
	int         numSurfaces, numBones, ofsBones, ofsSurfaces, i, s;
	int         numFrames, f, ofsFrames;
	shader_t    **shaders;

	if( skbLen < 92 || ReadInt( skb ) != SKEL_IDENT || ReadInt( skb + 4 ) != SKEL_VERSION )
	{
		ri.Printf( PRINT_WARNING, "R_LoadSkel: %s is not a v%d skeleton\n", name, SKEL_VERSION );
		return qfalse;
	}
	numSurfaces = ReadInt( skb + 72 );
	numBones = ReadInt( skb + 76 );
	ofsBones = ReadInt( skb + 80 );
	ofsSurfaces = ReadInt( skb + 84 );
	if( numBones < 1 || numBones > SKEL_MAXBONES || numSurfaces < 1 || numSurfaces > TIKI_MAX_SURFACES
	    || ofsBones + numBones * 72 > skbLen || ofsSurfaces >= skbLen )
	{
		ri.Printf( PRINT_WARNING, "R_LoadSkel: %s has a bad header\n", name );
		return qfalse;
	}

	if( skaLen < 108 || ReadInt( ska ) != SKEL_ANIM_IDENT || ReadInt( ska + 4 ) != SKEL_VERSION
	    || ReadInt( ska + 80 ) != numBones )
	{
		ri.Printf( PRINT_WARNING, "R_LoadSkel: animation for %s does not match the skeleton\n", name );
		return qfalse;
	}
	numFrames = ReadInt( ska + 76 );
	ofsFrames = ReadInt( ska + 104 );
	if( numFrames < 1 || numFrames > 4096 || ofsFrames + numFrames * ( 40 + numBones * 16 ) > skaLen )
	{
		ri.Printf( PRINT_WARNING, "R_LoadSkel: bad animation for %s\n", name );
		return qfalse;
	}

	sk = ri.Hunk_Alloc( sizeof( *sk ), h_low );
	sk->numBones = numBones;
	sk->scale = scale;
	sk->numFrames = numFrames;
	sk->frameTime = ReadFloat( ska + 92 );
	if( sk->frameTime <= 0 )
	{
		sk->frameTime = 1.0f / 15.0f;
	}
	for( i = 0; i < numBones; i++ )
	{
		sk->parent[i] = ReadInt( skb + ofsBones + i * 72 );
	}

	sk->quat = ri.Hunk_Alloc( numFrames * numBones * 4 * sizeof( float ), h_low );
	sk->offset = ri.Hunk_Alloc( numFrames * numBones * 3 * sizeof( float ), h_low );
	for( f = 0; f < numFrames; f++ )
	{
		const byte *fr = ska + ofsFrames + f * ( 40 + numBones * 16 ) + 40;

		for( i = 0; i < numBones; i++, fr += 16 )
		{
			int k;

			for( k = 0; k < 4; k++ )
			{
				sk->quat[( f * numBones + i ) * 4 + k] = LittleShort( *(const short *)( fr + k * 2 )) * QUAT_RECIP;
			}
			for( k = 0; k < 3; k++ )
			{
				sk->offset[( f * numBones + i ) * 3 + k] = LittleShort( *(const short *)( fr + 8 + k * 2 )) * OFFSET_RECIP;
			}
		}
	}

	sk->numSurfaces = numSurfaces;
	sk->surfaces = ri.Hunk_Alloc( numSurfaces * sizeof( tikiSkinSurface_t ), h_low );
	shaders = ri.Hunk_Alloc( numSurfaces * sizeof( *shaders ), h_low );

	{
		const byte *surf = skb + ofsSurfaces;

		for( s = 0; s < numSurfaces; s++ )
		{
			tikiSkinSurface_t *ss = &sk->surfaces[s];
			char          surfName[MAX_QPATH], shaderName[MAX_QPATH];
			const char    *shaderFile = NULL;
			int           numTris, numVerts, ofsTris, ofsVerts, ofsEnd;
			const byte    *v;

			if(( surf - skb ) + 96 > skbLen || ReadInt( surf ) != SKEL_IDENT )
			{
				ri.Printf( PRINT_WARNING, "R_LoadSkel: %s has a bad surface %d\n", name, s );
				return qfalse;
			}
			Q_strncpyz( surfName, (const char *)surf + 4, sizeof( surfName ));
			numTris = ReadInt( surf + 68 );
			numVerts = ReadInt( surf + 72 );
			ofsTris = ReadInt( surf + 80 );
			ofsVerts = ReadInt( surf + 84 );
			ofsEnd = ReadInt( surf + 92 );
			if( numTris < 1 || numVerts < 1 || ( surf - skb ) + ofsEnd > skbLen || ofsTris + numTris * 12 > ofsEnd )
			{
				ri.Printf( PRINT_WARNING, "R_LoadSkel: %s has a bad surface %d\n", name, s );
				return qfalse;
			}

			ss->numVerts = numVerts;
			ss->numTris = numTris;
			ss->indexes = ri.Hunk_Alloc( numTris * 3 * sizeof( int ), h_low );
			for( i = 0; i < numTris * 3; i++ )
			{
				ss->indexes[i] = ReadInt( surf + ofsTris + i * 4 );
				if( ss->indexes[i] < 0 || ss->indexes[i] >= numVerts )
				{
					ri.Printf( PRINT_WARNING, "R_LoadSkel: %s has a bad index\n", name );
					return qfalse;
				}
			}

			ss->verts = ri.Hunk_Alloc( numVerts * sizeof( tikiSkinVert_t ), h_low );
			v = surf + ofsVerts;
			for( i = 0; i < numVerts; i++ )
			{
				tikiSkinVert_t *sv = &ss->verts[i];
				int        w, nw;

				if(( v - skb ) + 24 > skbLen )
				{
					return qfalse;
				}
				sv->normal[0] = ReadFloat( v );
				sv->normal[1] = ReadFloat( v + 4 );
				sv->normal[2] = ReadFloat( v + 8 );
				sv->st[0] = ReadFloat( v + 12 );
				sv->st[1] = ReadFloat( v + 16 );
				nw = ReadInt( v + 20 );
				if( nw < 0 || nw > 16 || ( v - skb ) + 24 + nw * 20 > skbLen )
				{
					ri.Printf( PRINT_WARNING, "R_LoadSkel: %s has bad vertex weights\n", name );
					return qfalse;
				}
				sv->numWeights = nw;
				sv->weights = ri.Hunk_Alloc( nw * sizeof( tikiSkinWeight_t ) + 1, h_low );
				for( w = 0; w < nw; w++ )
				{
					const byte *wp = v + 24 + w * 20;

					sv->weights[w].bone = ReadInt( wp );
					sv->weights[w].weight = ReadFloat( wp + 4 );
					sv->weights[w].offset[0] = ReadFloat( wp + 8 );
					sv->weights[w].offset[1] = ReadFloat( wp + 12 );
					sv->weights[w].offset[2] = ReadFloat( wp + 16 );
					if( sv->weights[w].bone < 0 || sv->weights[w].bone >= numBones )
					{
						ri.Printf( PRINT_WARNING, "R_LoadSkel: %s has a bad bone index\n", name );
						return qfalse;
					}
				}
				v += 24 + nw * 20;
			}

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
			surf += ofsEnd;
		}
	}

	mod->type = MOD_SKEL;
	mod->skel = sk;
	mod->tanShaders = shaders;
	mod->numTanSurfaces = numSurfaces;
	return qtrue;
}

static void QuatToMatrix( const float *q, float m[9] )
{
	float x = q[0], y = q[1], z = q[2], w = q[3];
	float n = x * x + y * y + z * z + w * w;
	float x2, y2, z2;

	if( n > 0.0f )
	{
		n = 1.0f / sqrt( n );
		x *= n; y *= n; z *= n; w *= n;
	}
	x2 = x + x; y2 = y + y; z2 = z + z;

	// FAKK2 stores the rotation the other way round: this is the transpose of
	// the usual quaternion matrix (found by trying the conventions on a character)
	m[0] = 1 - ( y * y2 + z * z2 ); m[3] = x * y2 - w * z2;         m[6] = x * z2 + w * y2;
	m[1] = x * y2 + w * z2;         m[4] = 1 - ( x * x2 + z * z2 ); m[7] = y * z2 - w * x2;
	m[2] = x * z2 - w * y2;         m[5] = y * z2 + w * x2;         m[8] = 1 - ( x * x2 + y * y2 );
}

#define SKIN_MAX_VERTS 262144
#define SKIN_MAX_SURFS 4096

static drawVert_t     skinVerts[SKIN_MAX_VERTS];
static srfTriangles_t skinSurfs[SKIN_MAX_SURFS];

void R_AddSkelSurfaces( trRefEntity_t *ent )
{
	static int vertsUsed, surfsUsed, lastFrame = -1;
	model_t    *mod = tr.currentModel;
	skelModel_t *sk = mod->skel;
	float      R[SKEL_MAXBONES][9], O[SKEL_MAXBONES][3];
	qboolean   done[SKEL_MAXBONES];
	int        frame, i, j, s, left;

	if( tr.frameCount != lastFrame )
	{
		lastFrame = tr.frameCount;
		vertsUsed = surfsUsed = 0;
	}

	frame = ( int )(( tr.refdef.time + ent->e.shaderTime * 1000 ) * 0.001f / sk->frameTime ) % sk->numFrames;
	if( frame < 0 )
	{
		frame = 0;
	}

	// bone transforms: local rotation/offset are relative to the parent
	Com_Memset( done, 0, sizeof( done ));
	for( left = sk->numBones, j = 0; left > 0 && j <= sk->numBones; j++ )
	{
		for( i = 0; i < sk->numBones; i++ )
		{
			float local[9];
			const float *q = &sk->quat[( frame * sk->numBones + i ) * 4];
			const float *o = &sk->offset[( frame * sk->numBones + i ) * 3];
			int   p = sk->parent[i];

			if( done[i] || ( p >= 0 && ( p >= sk->numBones || !done[p] )))
			{
				continue;
			}
			QuatToMatrix( q, local );
			if( p < 0 )
			{
				Com_Memcpy( R[i], local, sizeof( local ));
				VectorCopy( o, O[i] );
			}
			else
			{
				int r, c, k;

				for( r = 0; r < 3; r++ )
				{
					for( c = 0; c < 3; c++ )
					{
						R[i][r * 3 + c] = 0;
						for( k = 0; k < 3; k++ )
						{
							R[i][r * 3 + c] += R[p][r * 3 + k] * local[k * 3 + c];
						}
					}
					O[i][r] = O[p][r] + R[p][r * 3] * o[0] + R[p][r * 3 + 1] * o[1] + R[p][r * 3 + 2] * o[2];
				}
			}
			done[i] = qtrue;
			left--;
		}
	}

	R_SetupEntityLighting( &tr.refdef, ent );

	for( s = 0; s < sk->numSurfaces; s++ )
	{
		tikiSkinSurface_t  *ss = &sk->surfaces[s];
		srfTriangles_t *tri;
		drawVert_t     *dv;

		if( vertsUsed + ss->numVerts > SKIN_MAX_VERTS || surfsUsed >= SKIN_MAX_SURFS )
		{
			return;
		}
		tri = &skinSurfs[surfsUsed++];
		dv = &skinVerts[vertsUsed];
		vertsUsed += ss->numVerts;

		Com_Memset( tri, 0, sizeof( *tri ));
		tri->surfaceType = SF_TRIANGLES;
		tri->numVerts = ss->numVerts;
		tri->numIndexes = ss->numTris * 3;
		tri->verts = dv;
		tri->indexes = ss->indexes;
		ClearBounds( tri->bounds[0], tri->bounds[1] );

		for( i = 0; i < ss->numVerts; i++, dv++ )
		{
			const tikiSkinVert_t *sv = &ss->verts[i];
			vec3_t           pos = { 0, 0, 0 }, nrm = { 0, 0, 0 };

			for( j = 0; j < sv->numWeights; j++ )
			{
				const tikiSkinWeight_t *w = &sv->weights[j];
				const float        *r = R[w->bone];
				vec3_t             p;

				p[0] = r[0] * w->offset[0] + r[1] * w->offset[1] + r[2] * w->offset[2] + O[w->bone][0];
				p[1] = r[3] * w->offset[0] + r[4] * w->offset[1] + r[5] * w->offset[2] + O[w->bone][1];
				p[2] = r[6] * w->offset[0] + r[7] * w->offset[1] + r[8] * w->offset[2] + O[w->bone][2];
				VectorMA( pos, w->weight, p, pos );

				nrm[0] += w->weight * ( r[0] * sv->normal[0] + r[1] * sv->normal[1] + r[2] * sv->normal[2] );
				nrm[1] += w->weight * ( r[3] * sv->normal[0] + r[4] * sv->normal[1] + r[5] * sv->normal[2] );
				nrm[2] += w->weight * ( r[6] * sv->normal[0] + r[7] * sv->normal[1] + r[8] * sv->normal[2] );
			}
			VectorScale( pos, sk->scale, dv->xyz );
			VectorNormalize2( nrm, dv->normal );
			dv->st[0] = sv->st[0];
			dv->st[1] = sv->st[1];
			dv->lightmap[0] = dv->lightmap[1] = 0;
			dv->color[0] = dv->color[1] = dv->color[2] = dv->color[3] = 255;
			AddPointToBounds( dv->xyz, tri->bounds[0], tri->bounds[1] );
		}
		VectorAdd( tri->bounds[0], tri->bounds[1], tri->localOrigin );
		VectorScale( tri->localOrigin, 0.5f, tri->localOrigin );
		tri->radius = Distance( tri->bounds[0], tri->localOrigin );

		R_AddDrawSurf(( surfaceType_t * )tri, mod->tanShaders[s], 0, 0 );
	}
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
	char          skbFile[MAX_QPATH] = "";
	char          skaFile[MAX_QPATH] = "";
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
					Q_strncpyz( skbFile, COM_ParseExt( &text, qfalse ), sizeof( skbFile ));
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
				else if( !skaFile[0] && strlen( token ) > 4 && !Q_stricmp( token + strlen( token ) - 4, ".ska" ))
				{
					Q_strncpyz( skaFile, token, sizeof( skaFile ));
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

	if( skeletal )
	{
		byte *skb, *ska;
		int  skbLen, skaLen;
		char file[MAX_QPATH];

		if( !skbFile[0] || !skaFile[0] || !path[0] )
		{
			return qfalse;
		}
		Com_sprintf( file, sizeof( file ), "%s/%s", path, skbFile );
		skbLen = ri.FS_ReadFile( file, (void **)&skb );
		if( skbLen <= 0 || !skb )
		{
			return qfalse;
		}
		Com_sprintf( file, sizeof( file ), "%s/%s", path, skaFile );
		skaLen = ri.FS_ReadFile( file, (void **)&ska );
		if( skaLen <= 0 || !ska )
		{
			ri.FS_FreeFile( skb );
			return qfalse;
		}
		ok = R_LoadSkel( mod, skb, skbLen, ska, skaLen, name, scale, path, surfs, numSurfs );
		ri.FS_FreeFile( skb );
		ri.FS_FreeFile( ska );
		return ok;
	}

	if( !tanFile[0] || !path[0] )
	{
		return qfalse;
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
