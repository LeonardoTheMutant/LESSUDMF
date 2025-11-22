//"Less UDMF" version 3
//A tool to optimize the UDMF maps data in WAD
//Code by LeonardoTheMutant

//Changes in version 3:
// - Sector merging algorithm was updated to detect sloped sectors in SRB2 format (made either by sector properties,
//   linedef specials or vertices) so they don't get merged with each other

// TODO in future updates:
// - Add support for different game engines (DOOM, Heretic, Hexen, Strife, ZDoom, etc.)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//for areFilesSame()
#if defined(_WIN32)
	#include <direct.h>
	#define realpath(N,R) _fullpath((R),(N),_MAX_PATH)
	#define PATHCMP _stricmp
#else
	#include <limits.h>
	#include <unistd.h>
	#define PATHCMP strcmp
#endif

//Game engines
enum {
	ENGINE_UNKNOWN, //unknown engine, some program features will be disabled
	ENGINE_DOOM, //currently unsupported
	ENGINE_HERETIC, //currently unsupported
	ENGINE_HEXEN,  //currently unsupported
	ENGINE_STRIFE, //currently unsupported
	ENGINE_ZDOOM,  //currently unsupported
	ENGINE_SRB2, //Sonic Robo Blast 2
};

//Lump
typedef struct {
	char name[8];
	int adress;
	int size;
} lump_t;

//Key/Value pair inside data block
typedef struct {
	char *key;
	char *value;
} field_t;

//single TEXTMAP data block
typedef struct {
	char header[8]; //"sector", "sidedef", "thing", etc.
	field_t *fields; //array of key/value fields
	size_t fieldsCount; //amount of fields the block has
	size_t fieldsCapacity; //allocated capacity of the .fields array
} block_t;

typedef struct {
	block_t *block; //pointer to the block
	char isMaster; // 1=kept, 0=removed as duplicate, -1=unvisited
	char isSlope; // 1=sloped sector, 0=not sloped
	int sectorID; //index of the sector in ORIGINAL ordering
	int masterID; // new index of the sector
} sector_t;

// Collection of (cross-compatible) default field values taken from all known engines
const field_t defaultFieldValues[] = {
	{"blocking", "false"},
	{"blockmonsters", "false"},
	{"twosided", "false"},
	{"dontpegtop", "false"},
	{"dontpegbottom", "false"},
	{"secret", "false"},
	{"blocksound", "false"},
	{"dontdraw", "false"},
	{"mapped", "false"},
	{"playercross", "false"},
	{"playeruse", "false"},
	{"monstercross", "false"},
	{"monsteruse", "false"},
	{"impact", "false"},
	{"playerpush", "false"},
	{"monsterpush", "false"},
	{"missilecross", "false"},
	{"repeatspecial", "false"},

	{"special", "0"},
	{"arg0", "0"},
	{"arg1", "0"},
	{"arg2", "0"},
	{"arg3", "0"},
	{"arg4", "0"},
	{"arg5", "0"},
	{"arg6", "0"},
	{"arg7", "0"},
	{"arg8", "0"},
	{"arg9", "0"},

	{"sideback", "-1"},
	{"alpha", "1.0"},
	{"renderstyle", "translucent"},

	{"offsetx", "0"},
	{"offsety", "0"},
	
	{"texturetop", "-"},
	{"texturebottom", "-"},
	{"texturemiddle", "-"},

	{"repeatcnt", "0"},

	{"scalex_top", "1.0"},
	{"scaley_top", "1.0"},
	{"scalex_mid", "1.0"},
	{"scaley_mid", "1.0"},
	{"scalex_bottom", "1.0"},
	{"scaley_bottom", "1.0"},
	{"offsetx_top", "0.0"},
	{"offsety_top", "0.0"},
	{"offsetx_mid", "0.0"},
	{"offsety_mid", "0.0"},
	{"offsetx_bottom", "0.0"},
	{"offsety_bottom", "0.0"},

	{"light", "0"},

	{"heightfloor", "0"},
	{"heightceiling", "0"},

	{"lightfloor", "0"},
	{"lightceiling", "0"},
	
	{"special", "0"},

	{"xpanningfloor", "0.0"},
	{"ypanningfloor", "0.0"},
	{"xpanningceiling", "0.0"},
	{"ypanningceiling", "0.0"},
	{"xscalefloor", "1.0"},
	{"yscalefloor", "1.0"},
	{"xscaleceiling", "1.0"},
	{"yscaleceiling", "1.0"},
	{"rotationfloor", "0.0"},
	{"rotationceiling", "0.0"},

	{"lightcolor", "0x000000"},
	{"lightalpha", "25"},
	{"fadecolor", "0x000000"},
	{"fadealpha", "25"},
	{"fadestart", "0"},
	{"fadeend", "31"},

	{"gravity", "1.0"},
	{"triggertag", "0"},

	{"height", "0"},
	{"angle", "0"},
	{"pitch", "0"},
	{"roll", "0"},
	{"scalex", "1.0"},
	{"scaley", "1.0"},
	{"scale", "1.0"},
	{"mobjscale", "1.0"},
	
	{"skill1", "false"},
	{"skill2", "false"},
	{"skill3", "false"},
	{"skill4", "false"},
	{"skill5", "false"},
	{"ambush", "false"},
	{"single", "false"},
	{"dm", "false"},
	{"coop", "false"},
	{NULL, NULL},
};

const char *sectorSlopeKeys_SRB2[] = {
	"floorplane_a", "floorplane_b", "floorplane_c", "floorplane_d",
	"ceilingplane_a", "ceilingplane_b", "ceilingplane_c", "ceilingplane_d",
	NULL
};

block_t *blocks;
unsigned int blockCount = 0;
sector_t *sectors;
unsigned int sectorCount;
char *namespaceStr;
char gameEngine;

static FILE *inputWAD;
static FILE *outputWAD;
static char outputFilePath[PATH_MAX] = "./OUTPUT.WAD";

static unsigned int bufferA = 0; //multipurpose
static unsigned int bufferB = 0; //multipurpose
static char buffer_str[0x400];
static char *LUMP_BUFFER;

static unsigned int WAD_LumpsAmount;
static unsigned int WAD_DirectoryAddress;
static lump_t *lumps; //array of lumps loaded from the Input Wad

//Check if two filepaths refer to the same file
char BOOL_AreSameFiles(const char *path1, const char *path2) {
	char abs1[PATH_MAX], abs2[PATH_MAX];
	if (!realpath(path1, abs1) || !realpath(path2, abs2))
		return 0; // If either path can't be resolved, assume not the same
	return !PATHCMP(abs1, abs2);
}

//Close I/O
static void closeIO(void) {
	if (inputWAD) { fclose(inputWAD); inputWAD = 0; }
	if (outputWAD) { fclose(outputWAD); outputWAD = 0; }
}

//isspace() from ctype.h
static char isspace(char c) {
	return (c == 0x20 || c == 0x09 || (c >= 0x0a && c <= 0x0d));
}

// Get the value for a key in a block
static const char *getFieldValueFromBlock(const block_t *blk, const char *key) {
	for (unsigned short i = 0; i < blk->fieldsCount; i++) {
		if (!strcmp(blk->fields[i].key, key)) return blk->fields[i].value;
	}
	return 0;
}

// Check if the block contains a field with the given key
static char BOOL_BlockHasField(const block_t *blk, const char *strkey) {
	for (unsigned short i = 0; i < blk->fieldsCount; i++) {
		if (!strcmp(blk->fields[i].key, strkey)) return 1;
	}
	return 0;
}

//Safe realloc()
static void *xrealloc(void *ptr, size_t size) {
	void *p = realloc(ptr, size);
	if (!p) {
		fprintf(stderr, "ERROR: Out of memory in realloc()\n");
		exit(1);
	}
	return p;
}

//Safe strdup()
static char *xstrdup(const char *s) {
	if (!s) return NULL;
	size_t len = strlen(s);
	char *d = (char*)malloc(len + 1);
	if (!d) {
		fprintf(stderr, "ERROR: Out of memory in strdup()\n");
		exit(1);
	}
	memcpy(d, s, len + 1);
	return d;
}

//Add a key/value field into block
static void addField(block_t *blk, const char *key, const char *value) {
	if (blk->fieldsCount >= blk->fieldsCapacity) {
		blk->fieldsCapacity = blk->fieldsCapacity ? blk->fieldsCapacity * 2 : 4;
		blk->fields = (field_t*)xrealloc(blk->fields, blk->fieldsCapacity * sizeof(field_t));
	}
	blk->fields[blk->fieldsCount].key = xstrdup(key);
	blk->fields[blk->fieldsCount].value = xstrdup(value);
	blk->fieldsCount++;
}

// Is a given key/value pair equal to a known default value? Multiengine checker
static char BOOL_IsFieldDefaultValue(const field_t *a) {
	for (unsigned char x = 0; defaultFieldValues[x].key; x++)
		if (!strcmp(defaultFieldValues[x].key, a->key) && !strcmp(defaultFieldValues[x].value, a->value)) return 1;

	return 0;
}

//Compare two block_t structs
static char BOOL_AreBlocksEqual(const block_t *a, const block_t *b) {
	if (a->fieldsCount != b->fieldsCount) return 0;
	char matched[b->fieldsCount];
	memset(matched, 0, sizeof(matched));
	for (unsigned short i = 0; i < a->fieldsCount; i++) {
		int found = 0;
		for (unsigned short j = 0; j < b->fieldsCount; j++) {
			if (!matched[j] && !strcmp(a->fields[i].key, b->fields[j].key) && !strcmp(a->fields[i].value, b->fields[j].value)) {
				matched[j] = 1;
				found = 1;
				break;
			}
		}
		if (!found) return 0;
	}
	return 1;
}

// Collect linedef blocks that reference any sidedef belonging to the given sector.
// Returns a dynamically allocated array of block_t* and sets outCount. Caller must free the
// returned array (but not the block_t pointers themselves).
static block_t **SECTOR_GetLinedefs(unsigned int sectorIndex, unsigned int *outCount) {
	*outCount = 0;
	if (sectorIndex >= sectorCount) return NULL;

	// First pass: find which sidedef indices point to this sector
	bufferA = 0; //side index
	bufferB = 0; //matching count
	unsigned int *matchingSides = NULL;
	snprintf(buffer_str, sizeof(buffer_str), "%u", sectorIndex);

	for (unsigned int i = 0; i < blockCount; i++) {
		if (!strncmp(blocks[i].header, "sidedef", 7)) {
			const char *sec = getFieldValueFromBlock(&blocks[i], "sector");
			if (sec) {
				unsigned int sval = strtol(sec, NULL, 10);
				if (sval >= 0 && sval == sectorIndex) {
					matchingSides = (unsigned int*)xrealloc(matchingSides, (bufferB + 1) * sizeof(unsigned int));
					matchingSides[bufferB++] = bufferA;
				}
			}
			bufferA++;
		}
	}

	if (!bufferB) { //found no sidedefs referencing this sector
		free(matchingSides);
		return NULL;
	}

	// Second pass: find linedefs that reference any of these sidedef indices
	block_t **found = NULL;
	bufferA = 0; //found linedefs count
	for (unsigned int i = 0; i < blockCount; i++) {
		if (!strncmp(blocks[i].header, "linedef", 7)) {
			// examine each pair in the linedef; if any numeric value equals a matching sidedef index, record it
			for (unsigned short p = 0; p < blocks[i].fieldsCount; p++) {
				// check for sidedef references
				if (!strncmp(blocks[i].fields[p].key, "sidefront", 9) || !strncmp(blocks[i].fields[p].key, "sideback", 8)) {
					unsigned int iv = strtol(blocks[i].fields[p].value, NULL, 10);
					if (iv >= 0) {
						// compare against matchingSides
						for (unsigned int m = 0; m < bufferB; m++) {
							if (iv == matchingSides[m]) {
								found = (block_t**)xrealloc(found, (bufferA + 1) * sizeof(block_t*));
								found[bufferA++] = &blocks[i];
								goto next_linedef; // avoid adding same linedef multiple times
							}
						}
					}
				}
			}
		}
		next_linedef: ;
	}

	free(matchingSides);
	*outCount = bufferA;
	return found;
}

// Collect unique vertex blocks that form the polygon boundary of the given sector.
// Returns a dynamically allocated array of block_t* (unique, order of discovery) and sets outCount.
// Caller must free the returned array (but not the block_t pointers themselves).
static block_t **SECTOR_GetPolygonVertices(unsigned int sectorIndex, unsigned int *outCount) {
	*outCount = 0;
	if (sectorIndex >= sectorCount) return NULL;

	// First pass: collect sidedef indices belonging to this sector
	bufferA = 0; //side Idx
	bufferB = 0; //sidedef count
	unsigned int *sidedefIndices = NULL;
	for (unsigned int i = 0; i < blockCount; i++) {
		if (!strncmp(blocks[i].header, "sidedef", 7)) {
			const char *sec = getFieldValueFromBlock(&blocks[i], "sector");
			if (sec) {
				char tmp[64]; size_t sl = strlen(sec);
				if (sl >= sizeof(tmp)) sl = sizeof(tmp)-1;
				memcpy(tmp, sec, sl); tmp[sl] = '\0';
				if (tmp[0] == '"' && tmp[sl-1] == '"') { memmove(tmp, tmp+1, sl-2); tmp[sl-2] = '\0'; }
				// trim trailing and leading spaces
				{
					char *end = tmp + strlen(tmp) - 1;
					while (end >= tmp && isspace((unsigned char)*end)) { *end = '\0'; end--; }
				}
				char *t = tmp; while (*t && isspace((unsigned char)*t)) t++;
				char *endptr = NULL;
				long sval = strtol(t, &endptr, 10);
				if (endptr && *endptr == '\0' && sval >= 0 && (unsigned long)sval == (unsigned long)sectorIndex) {
					sidedefIndices = (unsigned int*)xrealloc(sidedefIndices, (bufferB + 1) * sizeof(unsigned int));
					sidedefIndices[bufferB++] = bufferA;
				}
			}
			bufferA++;
		}
	}
	if (!bufferB) { //found no sidedefs
		free(sidedefIndices);
		return NULL;
	}

	// Build vertex list (ordered by occurrence) to index into
	bufferA = 0; //total vertices
	for (unsigned int i = 0; i < blockCount; i++) if (!strncmp(blocks[i].header, "vertex", 6)) bufferA++;
	if (!bufferA) { //
		free(sidedefIndices);
		return NULL;
	}

	block_t **vertexList = (block_t**)malloc(bufferA * sizeof(block_t*));
	unsigned int vi = 0;
	for (unsigned int i = 0; i < blockCount; i++) if (!strncmp(blocks[i].header, "vertex", 6)) vertexList[vi++] = &blocks[i];

	// Second pass: for every linedef, if it references any of the sidedefIndices, collect its v1/v2
	block_t **found = NULL;
	unsigned int foundCount = 0;
	for (unsigned int i = 0; i < blockCount; i++) {
		if (!strncmp(blocks[i].header, "linedef", 7)) {
			// check fields for sidefront/sideback
			char referencesSector = 0;
			for (unsigned short p = 0; p < blocks[i].fieldsCount; ++p) {
				if (!strncmp(blocks[i].fields[p].key, "sidefront", 9) || !strncmp(blocks[i].fields[p].key, "sideback", 8)) {
					unsigned int iv = strtol(blocks[i].fields[p].value, NULL, 10);
					if (iv >= 0) {
						for (unsigned int m = 0; m < bufferB; m++) {
							if (iv == sidedefIndices[m]) { referencesSector = 1; break; }
						}
					}
					if (referencesSector) break;
				}
			}
			if (!referencesSector) continue;

			// get v1 and v2 fields
			const char *verts[2] = { getFieldValueFromBlock(&blocks[i], "v1"), getFieldValueFromBlock(&blocks[i], "v2") };
			for (int viidx = 0; viidx < 2; viidx++) {
				const char *vs = verts[viidx];
				if (!vs || !*vs) continue;
				unsigned int idx = strtol(vs, NULL, 10);
				if (idx >= 0 && idx < bufferA) {
					// avoid duplicates
					char already = 0;
					for (unsigned int f = 0; f < foundCount; f++) if (found[f] == vertexList[idx]) { already = 1; break; }
					if (!already) {
						found = (block_t**)xrealloc(found, (foundCount + 1) * sizeof(block_t*));
						found[foundCount++] = vertexList[idx];
					}
				}
			}
		}
	}

	free(sidedefIndices);
	free(vertexList);
	*outCount = foundCount;
	return found;
}

// Determine whether a sector is likely a sloped sector by checking for slope-related fields
// in the sector itself, related linedefs and vertices.
// Argument is an index to the sectors[] array
static char BOOL_IsSectorSloped(unsigned int sectorIndex) {
	if (sectorIndex >= sectorCount) return 0;
	block_t *sectorBlk = sectors[sectorIndex].block;

	// Check if sector has fields directly indicating that it is a slope
	const char **sectorKeys = NULL;
	switch(gameEngine) {
		case ENGINE_SRB2:
			sectorKeys = sectorSlopeKeys_SRB2;
			break;
		default:
			sectorKeys = NULL;
			break;
	}
	if (sectorKeys) {
		for (int k = 0; sectorKeys[k]; k++) {
			if (BOOL_BlockHasField(sectorBlk, sectorKeys[k])) return 1;
		}
	}

	// Collect linedefs associated with this sector (via sidedefs)
	bufferA = 0; //linedef count
	block_t **linedefs = SECTOR_GetLinedefs(sectorIndex, &bufferA);
	if (!linedefs) {
		return 0; // no linedefs referencing sidedefs of this sector
	}

	// Inspect collected linedefs for slope-creating properties (conservative)
	for (unsigned int i = 0; i < bufferA; i++) {
		block_t *ld = linedefs[i];
		const char *specs = getFieldValueFromBlock(ld, "special");
		bufferB = strtol(specs ? specs : "0", NULL, 10); //Linedef special number
		switch (gameEngine)
		{
			case ENGINE_SRB2: //Sonic Robo Blast 2
				switch(bufferB) {
					case 700:
					case 704:
					case 720:
					case 799:
						free(linedefs);
						return 1;
				}
				break;
		}
	}

	if (gameEngine == ENGINE_SRB2) {
		// Polygon-only vertex check: collect unique polygon vertices for the sector and inspect them.
		// Stricter rule: require at least two distinct numeric vertex heights to mark the sector sloped.
		bufferA = 0; //polyVertexCount
		block_t **polyVertices = SECTOR_GetPolygonVertices(sectorIndex, &bufferA);
		if (polyVertices) {

			double *zvals = NULL;
			unsigned int zcount = 0;

			for (unsigned int pv = 0; pv < bufferA; pv++) {

				const char *zstr = NULL;
				if (BOOL_BlockHasField(polyVertices[pv], "zfloor")) zstr = getFieldValueFromBlock(polyVertices[pv], "zfloor");
				else if (BOOL_BlockHasField(polyVertices[pv], "zceiling")) zstr = getFieldValueFromBlock(polyVertices[pv], "zceiling");

				if (zstr && *zstr) {
					double zv = strtod(zstr, NULL);
					char found = 0;

					for (unsigned int zi = 0; zi < zcount; zi++)
						if (zvals && zvals[zi] == zv) { found = 1; break; }

					if (!found) {
						zvals = (double*)xrealloc(zvals, (zcount + 1) * sizeof(double));
						zvals[zcount++] = zv;
					}
				}
			}

			if (zcount > 0) {
				free(zvals);
				free(polyVertices);
				free(linedefs);
				return 1;
			}

			free(zvals);
			free(polyVertices);
		}
	}

	free(linedefs);
	return 0;
}

static void MAP_DeduplicateSectors() {
	sectorCount = 0;
	//Count the amount of sectors in map
	for (unsigned int i = 0; i < blockCount; i++) {
		if (!strncmp(blocks[i].header, "sector", 6)) sectorCount++;
	}
	sectors = (sector_t*)calloc(sectorCount, sizeof(sector_t)); //Allocate the space for sectors
	//Load sectors
	bufferA = 0; //Sector counter
	for (unsigned int i = 0; i < blockCount; i++) {
		if (!strncmp(blocks[i].header, "sector", 6)) sectors[bufferA++].block = &blocks[i];
	}
	// Mark all sectors as unvisited by the program
	for (unsigned int i = 0; i < sectorCount; i++) sectors[i].isMaster = -1;

	// Precompute slope flags for all sectors so we don't merge sloped sectors
	for (unsigned int si = 0; si < sectorCount; si++) sectors[si].isSlope = 0;
	for (unsigned int si = 0; si < sectorCount; si++) sectors[si].isSlope = BOOL_IsSectorSloped(si);

	//Find identical sectors and mark them
	unsigned int uniqueSectorID = 0;
	for (unsigned int i = 0; i < sectorCount; i++) {
		if (sectors[i].isMaster != -1) continue; //Skip sector because it is already processed

		sectors[i].isMaster = 1;
		sectors[i].masterID = uniqueSectorID;
		sectors[i].sectorID = i;

		//Compare with other sectors
		for (unsigned int j = i + 1; j < sectorCount; j++) {
			// Skip if already processed or either sector is a slope (do not merge slopes)
			if (sectors[j].isMaster != -1 || sectors[j].isSlope || sectors[i].isSlope) {
				continue;
			}

			if (BOOL_AreBlocksEqual(sectors[i].block, sectors[j].block)) {
				//Found a duplicate
				sectors[j].masterID = uniqueSectorID;
				sectors[j].sectorID = j;
				sectors[j].isMaster = 0; //Mark as duplicate
			}
		}

		uniqueSectorID++;
	}

	//Count the amount of sidedefs in map
	block_t *sidedefs;
	unsigned int sidedefCount = 0;
	for (unsigned int i = 0; i < blockCount; i++) {
		if (!strncmp(blocks[i].header, "sidedef", 7)) sidedefCount++;
	}
	//Load Sidedefs
	sidedefs = (block_t*)malloc(sizeof(block_t) * sidedefCount); //Allocate space for sidedefs
	bufferA = 0; //Sidedef counter
	for (unsigned int i = 0; i < blockCount; i++) {
		if (!strncmp(blocks[i].header, "sidedef", 7)) sidedefs[bufferA++] = blocks[i];
	}

	// Build masterID -> new compacted index
	int *masterID_to_newIndex = (int*)malloc(uniqueSectorID * sizeof(int));
	bufferA = 0; //new index counter
	for (unsigned int i = 0; i < sectorCount; i++) {
		if (sectors[i].isMaster == 1) masterID_to_newIndex[sectors[i].masterID] = bufferA++;
	}
	int *oldToNew = (int*)malloc(sectorCount * sizeof(int));
	for (unsigned int i = 0; i < sectorCount; i++) oldToNew[i] = masterID_to_newIndex[sectors[i].masterID];
	free(masterID_to_newIndex);
	
	// Remap sidedef sector indices
	for (unsigned int i = 0; i < sidedefCount; i++) {
		for (unsigned short j = 0; j < sidedefs[i].fieldsCount; j++) {
			if (!strncmp(sidedefs[i].fields[j].key, "sector", 6)) {
				unsigned int sectorIndex = strtol(sidedefs[i].fields[j].value, NULL, 10);
				
				if (sectorIndex < sectorCount) {
					snprintf(buffer_str, sizeof(buffer_str), "%d", oldToNew[sectorIndex]);
					strcpy(sidedefs[i].fields[j].value, buffer_str);
				} else {
					fprintf(stderr, "WARNING: Invalid or out-of-bounds sector index '%s' for sidedef, setting to 0\n", sidedefs[i].fields[j].value);
					strcpy(sidedefs[i].fields[j].value, "0");
				}
			}
		}
	}
	free(oldToNew);

	//Remove sector duplicates
	unsigned int writeIndex = 0;
	bufferA = 0; //track which sector we're looking at in sector[]

	for (unsigned int i = 0; i < blockCount; i++) {
		if (strncmp(blocks[i].header, "sector", 6)) {
			//Not a sector block, just keep the block
			if (writeIndex != i) blocks[writeIndex] = blocks[i];
			writeIndex++;
		} else {
			if (sectors[bufferA].isMaster) {
				//Master sector, keep the block
				if (writeIndex != i) blocks[writeIndex] = blocks[i];
				writeIndex++;
			} else {
				//Duplicate sector, free its fields and the block itself
				for (unsigned short j = 0; j < blocks[i].fieldsCount; j++) {
					free(blocks[i].fields[j].key);
					free(blocks[i].fields[j].value);
				}
				free(blocks[i].fields);
			}
			bufferA++;
		}
	}

	blockCount = writeIndex;

	printf("Merged the identical sectors in map (before: %d, after: %d)\n", sectorCount, uniqueSectorID);

	free(sectors);
	free(sidedefs);
}

//Tokenize TEXTMAP into block structures (block_t)
static void TEXTMAP_Parse(char *textmapdata) {
	char *ptr = textmapdata;
	while (*ptr) {
		// skip whitespace and comments at top-level
		if (*ptr == '/' && ptr[1] == '/') {
			ptr += 2; while (*ptr && *ptr != '\n') ptr++; if (*ptr) ptr++; continue;
		}
		if (*ptr == '/' && ptr[1] == '*') {
			ptr += 2; while (*ptr && !(ptr[0] == '*' && ptr[1] == '/')) ptr++; if (*ptr) ptr += 2; continue;
		}
		if (isspace((unsigned char)*ptr)) { ptr++; continue; }

		// namespace
		if (!strncmp(ptr, "namespace", 9)) {
			ptr += 9;
			while (isspace((unsigned char)*ptr)) ptr++;
			if (*ptr == '=') ptr++;
			while (isspace((unsigned char)*ptr)) ptr++;
			if (*ptr == '"') {
				ptr++;
				char *start = ptr;
				while (*ptr && *ptr != '"') ptr++;
				size_t len = ptr - start;
				namespaceStr = (char*)malloc(len + 1);
				memcpy(namespaceStr, start, len);
				namespaceStr[len] = '\0';
				if (*ptr == '"') ptr++;
			}
			// determine engine
			if (namespaceStr) {
				if (!strncmp(namespaceStr, "doom", 4)) gameEngine = ENGINE_DOOM;
				else if (!strncmp(namespaceStr, "heretic", 7)) gameEngine = ENGINE_HERETIC;
				else if (!strncmp(namespaceStr, "hexen", 5)) gameEngine = ENGINE_HEXEN;
				else if (!strncmp(namespaceStr, "strife", 6)) gameEngine = ENGINE_STRIFE;
				else if (!strncmp(namespaceStr, "zdoom", 5)) gameEngine = ENGINE_ZDOOM;
				else if (!strncmp(namespaceStr, "srb2", 4)) gameEngine = ENGINE_SRB2;
				else gameEngine = ENGINE_UNKNOWN;
			}
			// skip until semicolon
			while (*ptr && *ptr != ';') ptr++;
			if (*ptr == ';') ptr++;
			continue;
		}

		// Read block header (skip leading whitespace and avoid consuming comments)
		char headerBuf[64]; int hi = 0;

		// read header token until whitespace, '{' or comment start
		while (*ptr && *ptr != '{') {
			if ((*ptr == '/' && (ptr[1] == '/' || ptr[1] == '*')) || isspace((unsigned char)*ptr)) break; // don't include comment start or whitespace in header
			if (hi < (int)sizeof(headerBuf)-1) headerBuf[hi++] = *ptr;
			ptr++;
		}
		headerBuf[hi] = '\0';

		// If we didn't read a header (e.g. encountered comment or stray chars), skip comments and continue
		if (!hi) {
			// skip comments or stray characters until next top-level token
			if (*ptr == '/' && ptr[1] == '/') { ptr += 2; while (*ptr && *ptr != '\n') ptr++; if (*ptr) ptr++; continue; }
			if (*ptr == '/' && ptr[1] == '*') { ptr += 2; while (*ptr && !(ptr[0] == '*' && ptr[1] == '/')) ptr++; if (*ptr) ptr += 2; continue; }
			// if we hit '{' without a header, just skip it
			if (*ptr == '{') { ptr++; continue; }
			// otherwise continue to next iteration
			continue;
		}

		//Allocate space for the new block_t and add new block to the memory
		blocks = (block_t*)realloc(blocks, (blockCount + 1) * sizeof(block_t));
		block_t *blk = &blocks[blockCount];
		memset(blk, 0, sizeof(block_t));
		strncpy(blk->header, headerBuf, sizeof(blk->header)-1);

		// skip whitespace/comments between header and '{'
		while (*ptr) {
			if (isspace((unsigned char)*ptr)) { ptr++; continue; }
			if (*ptr == '/' && ptr[1] == '/') { ptr += 2; while (*ptr && *ptr != '\n') ptr++; if (*ptr) ptr++; continue; }
			if (*ptr == '/' && ptr[1] == '*') { ptr += 2; while (*ptr && !(ptr[0] == '*' && ptr[1] == '/')) ptr++; if (*ptr) ptr += 2; continue; }
			break;
		}
		if (*ptr == '{') ptr++;

		// Parse fields inside block
		while (*ptr) {
			// skip whitespace and comments
			if (*ptr == '/' && ptr[1] == '/') { ptr += 2; while (*ptr && *ptr != '\n') ptr++; if (*ptr) ptr++; continue; }
			if (*ptr == '/' && ptr[1] == '*') { ptr += 2; while (*ptr && !(ptr[0]=='*' && ptr[1]=='/')) ptr++; if (*ptr) ptr += 2; continue; }
			if (isspace((unsigned char)*ptr)) { ptr++; continue; }
			if (*ptr == '}') { ptr++; break; }

			// read key
			char key[128]; int ki = 0;
			while (*ptr && *ptr != '=' && *ptr != '}' && !isspace((unsigned char)*ptr)) {
				if (ki < (int)sizeof(key)-1) key[ki++] = *ptr;
				ptr++;
			}
			key[ki] = '\0';
			while (isspace((unsigned char)*ptr)) ptr++;
			if (*ptr == '=') ptr++;
			while (isspace((unsigned char)*ptr)) ptr++;

			// read value
			char value[1024]; int vi = 0;
			if (*ptr == '"') {
				value[vi++] = '"'; ptr++;
				while (*ptr && *ptr != '"') { if (vi < (int)sizeof(value)-1) value[vi++] = *ptr; ptr++; }
				if (*ptr == '"') { if (vi < (int)sizeof(value)-1) value[vi++] = '"'; ptr++; }
			} else {
				while (*ptr && *ptr != ';' && *ptr != '}') { if (vi < (int)sizeof(value)-1) value[vi++] = *ptr; ptr++; }
			}
			value[vi] = '\0';

			addField(blk, key, value);

			// advance past semicolon if present
			if (*ptr == ';') ptr++;
		}

		blockCount++;
	}

	// Remove trailing empty blocks
	while (blockCount > 0 && blocks[blockCount - 1].fieldsCount == 0) {
		free(blocks[blockCount - 1].fields);
		blockCount--;
	}
}

// Generate a new TEXTMAP using the blocks data from memory
static char* TEXTMAP_GenerateNewDataFromBlocks(block_t *blocks) {
	size_t allocated = 0x100000; //1 Megabyte
	size_t used = 0;
	char *out = (char*)malloc(allocated);
	used += snprintf(out, allocated, "namespace=\"%s\";", namespaceStr);
	char blockLine[0x200];
	for (unsigned int block = 0; block < blockCount; block++) {
		memset(blockLine, 0, sizeof(blockLine));
		//Make a line with block's data
		for (unsigned int pair = 0; pair < blocks[block].fieldsCount; pair++) {
			if (!BOOL_IsFieldDefaultValue(&blocks[block].fields[pair])) { //only write fields which are not set to default
				snprintf(buffer_str, sizeof(buffer_str), "%s=%s;", blocks[block].fields[pair].key, blocks[block].fields[pair].value);
				strcat(blockLine, buffer_str);
			}
		}
		//Write the block itself with the data
		used += snprintf(buffer_str, sizeof(blockLine), "%s{%s}", blocks[block].header, blockLine);
		if (used >= allocated) { //reached the allocated limit, allocate more space
			allocated *= 2;
			out = (char*)xrealloc(out, allocated);
		}
		strncat(out, buffer_str, allocated);
	}
	strcat(out, "\n");
	return out;
}


//
// MAIN
//
int main(int argc, char *argv[]) {
	puts("LESSUDMF v3.0 by LeonardoTheMutant\n");

	if (argc < 2) {
		printf("%s [input.wad] -o [output.wad] -nm\n", argv[0]);
		puts("Optimize the UDMF maps data in WAD\n");
		printf("    -o\tOutput to the file. If not given, the output will be written to %s\n", outputFilePath);
		puts("    -nm\tDo not merge identical sectors in maps");
		puts("\nAlways make sure to have a copy of the old file - new file can have corruptions!");
		return 0;
	}

	// parse args
	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "-o", 2) == 0 && i + 1 < argc) strncpy(outputFilePath, argv[++i], sizeof(outputFilePath) - 1);
		else if (strncmp(argv[i], "-nm", 3) == 0) bufferA |= 2; //"No merge" option
		else strncpy(buffer_str, argv[i], sizeof(buffer_str));
	}

	if (BOOL_AreSameFiles(buffer_str, outputFilePath)) { fprintf(stderr, "ERROR: Input and Output files are the same, please choose other Output file\n"); return 1; }

	inputWAD = fopen(buffer_str, "rb");
	if (!inputWAD) { fprintf(stderr, "ERROR: Failed to open Input WAD (%s)\n", buffer_str); return 1; }
	outputWAD = fopen(outputFilePath, "wb");
	if (!outputWAD) { fprintf(stderr, "ERROR: Failed to open Output WAD (%s)\n", outputFilePath); closeIO(); return 1; }

	//Read the WAD type
	if (fread(buffer_str, 1, 4, inputWAD) != 4) { fprintf(stderr, "ERROR: Bad Input WAD header\n"); closeIO(); return 1; }
	fwrite(buffer_str, 1, 4, outputWAD);

	//Get the amount of lumps in WAD and allocate the space for them
	fread(&WAD_LumpsAmount, 4, 1, inputWAD);
	fwrite(&WAD_LumpsAmount, 4, 1, outputWAD);
	lumps = (lump_t*)malloc(sizeof(lump_t) * WAD_LumpsAmount);
	if (!lumps) { fprintf(stderr, "ERROR: Failed to allocate memory for the WAD lumps buffer"); closeIO(); return 1; }

	//Get Directory Table address
	fread(&WAD_DirectoryAddress, 4, 1, inputWAD);

	// Prepare to write new WAD: write placeholder for the Directory Table address (we'll correct it at the end)
	putc(0x0C, outputWAD);
	for (char x=0; x<3; x++) putc(0, outputWAD);

	// Seek to dir table, read it
	fseek(inputWAD, WAD_DirectoryAddress, SEEK_SET);
	puts("\nDirectory Table of the Input WAD:");
	puts("\nID   ADRESS     SIZE     NAME");
	for (unsigned int i = 0; i < WAD_LumpsAmount; i++) {
		fread(&lumps[i].adress, 4, 1, inputWAD);
		fread(&lumps[i].size, 4, 1, inputWAD);
		fread(lumps[i].name, 8, 1, inputWAD);
		printf("%2d %8d %8d %8s\n", i, lumps[i].adress, lumps[i].size, lumps[i].name);
	}

	// Copy/modify lumps
	fseek(inputWAD, 0x0C, SEEK_SET); //Jump back to the actuall lump data
	for (unsigned int i = 0; i < WAD_LumpsAmount; i++) {
		// Set new lump address
		lumps[i].adress = ftell(outputWAD);
		if (strncmp(lumps[i].name, "TEXTMAP", 7)) {
			//Lump is not TEXTMAP, copy the lump contents to the Output WAD unmodified
			if (lumps[i].size > 0) {
				LUMP_BUFFER = (char*)malloc(lumps[i].size);
				fread(LUMP_BUFFER, lumps[i].size, 1, inputWAD);
				fwrite(LUMP_BUFFER, lumps[i].size, 1, outputWAD);
				free(LUMP_BUFFER);
			}
		} else {
			//---------- Modify TEXTMAP ----------
			printf("\nWorking on TEXTMAP of %s...\n", lumps[i-1].name);

			//Copy TEXTMAP to memory
			LUMP_BUFFER = (char*)malloc(lumps[i].size + 1);
			fread(LUMP_BUFFER, lumps[i].size, 1, inputWAD);
			LUMP_BUFFER[lumps[i].size] = '\0';

			// Free old blocks if any
			for (unsigned int b = 0; b < blockCount; b++) {
				for (size_t p = 0; p < blocks[b].fieldsCount; p++) {
					free(blocks[b].fields[p].key);
					free(blocks[b].fields[p].value);
				}
				free(blocks[b].fields);
			}
			free(blocks);
			blocks = 0;
			blockCount = 0;

			//Parse the TEXTMAP into data blocks for the program
			TEXTMAP_Parse(LUMP_BUFFER);
			printf("Analized the map, namespace is \"%s\"\n", namespaceStr);
			free(LUMP_BUFFER); //Unload the lump because we no longer need the original data

			// Deduplicate (merge) identical sectors (enabled by default; disabled with -nm)
			if (!(bufferA & 2)) MAP_DeduplicateSectors();

			LUMP_BUFFER = TEXTMAP_GenerateNewDataFromBlocks(blocks); //Write new lump to the buffer
			lumps[i].size = strlen(LUMP_BUFFER);

			// Write the new TEXTMAP to the Output WAD
			fwrite(LUMP_BUFFER, lumps[i].size, 1, outputWAD);
			printf("Wrote the modified TEXTMAP data of %s to the output\n", lumps[i-1].name);

			free(LUMP_BUFFER);
		}
	}

	// Directory Table address
	bufferA = ftell(outputWAD); //where we are in the output WAD
	fseek(outputWAD, 8, SEEK_SET); //go back to the Directory Table address pointer in WAD
	fwrite(&bufferA, 4, 1, outputWAD); //write the address
	fseek(outputWAD, bufferA, SEEK_SET); //get back to the Directory Table

	//Write the new Directory Table
	puts("\nDirectory Table of the Output WAD:");
	puts("\nID   ADRESS     SIZE     NAME");
	for (unsigned int i = 0; i < WAD_LumpsAmount; i++) {
		printf("%2d %8d %8d %8s\n", i, lumps[i].adress, lumps[i].size, lumps[i].name);
		fwrite(&lumps[i].adress, 4, 1, outputWAD);
		fwrite(&lumps[i].size, 4, 1, outputWAD);
		fwrite(lumps[i].name, 8, 1, outputWAD);
	}

	closeIO();
	free(lumps);

	printf("\n\"%s\" is ready. Make sure to check the contents of the WAD for corruptions!\n", outputFilePath);
	return 0;
}
