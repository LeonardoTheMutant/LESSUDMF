//"Less UDMF" version 4
//A tool to optimize the UDMF maps data in WAD
//Code by LeonardoTheMutant

//Changes in version 4:
// - Improved the support for different game engines by implementing config files that describe the game
// - Fixed several bugs and improved memory management
// - Added functionality to remove unnecessary textures from Control Linedefs
// - Added functionality to remove angle information for no-angle things, since it is unnecessary
// - Improved the functionality of default valued UDMF fields removal

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#include "json.h"

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

//Internal program flags
enum flags {
	FLAG_CONFIGLOADED		= 1,	//Game configuration file is loaded
	FLAGS_CUSTOMCONFIG		= 2,	//Custom game configuration file is used
	FLAG_PRESERVESECTORS	= 4,	//Preserve indentical sectors in map
	FLAG_PRESERVETEXTURES	= 8,	//Preserve textures on control linedefs which do not use them
	FLAG_PRESERVEANGLES		= 16,	//Preserge facing angles on things that do not require angle information
	FLAG_PRESERVEDEFAULT	= 32,	//Preserve the UDMF fields which are set to default value
	FLAG_BIT7				= 64,	//Unused
	FLAG_BIT8				= 128	//Unused
};

enum configFlags {
	CFGFLAG_POLYGONSLOPE	= 1,	//Game supports slopes made with vertexes in triangular sectors
	CFGFLAG_BIT1			= 2,	//Unused
	CFGFLAG_BIT2			= 4,	//Unused
	CFGFLAG_BIT3			= 8,	//Unused
	CFGFLAG_BIT4			= 16,	//Unused
	CFGFLAG_BIT5			= 32,	//Unused
	CFGFLAG_BIT6			= 64,	//Unused
	CFGFLAG_BIT7			= 128,	//Unused
};

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

//Config files
const char *configFiles[] = {
	[ENGINE_UNKNOWN] = 0,
	[ENGINE_DOOM] = "./DOOM.JSON",
	[ENGINE_HERETIC] = "./HERETIC.JSON",
	[ENGINE_HEXEN] = "./HEXEN.JSON",
	[ENGINE_STRIFE] = "./STRIFE.JSON",
	[ENGINE_ZDOOM] = "./ZDOOM.JSON",
	[ENGINE_SRB2] = "./SRB2.JSON"
};

//Level elements
enum {
	LEVEL_VERTEX,
	LEVEL_LINEDEF,
	LEVEL_SIDEDEF,
	LEVEL_SECTOR,
	LEVEL_THING
};

//Lump
typedef struct {
	char name[8];
	uint32_t adress;
	uint32_t size;
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
	uint8_t fieldsCount; //amount of fields the block has
} block_t;

typedef struct {
	block_t *block; //pointer to the block
	char isMaster; // 1=kept, 0=removed as duplicate, -1=unvisited
	char isSlope; // 1=sloped sector, 0=not sloped
	int sectorID; //index of the sector in ORIGINAL ordering
	int masterID; // new index of the sector
} sector_t;

typedef struct {
	uint32_t filesize;
	struct stat filestatus;
	char *buffer;
	json_value *json;
	uint16_t *linedefSpecialsNoTexture;
	uint16_t *linedefSpecialsSlope;
	char **sectorFieldsSlope;
	uint16_t *thingTypesNoAngle;
	field_t *defaultValues[5];
	uint8_t flags;
} config_t;

block_t *blocks;
uint32_t blockCount = 0;
sector_t *sectors;
uint32_t sectorCount;
char *namespaceStr;
uint8_t gameEngine;
uint8_t gameEngine_last = UINT8_MAX;

static FILE *inputWAD;
static FILE *outputWAD;
static char outputFilePath[PATH_MAX] = "./OUTPUT.WAD";
static FILE *configFile;
config_t config;

static uint32_t bufferA = 0; //multipurpose
static uint32_t bufferB = 0; //multipurpose
static char buffer_str[0x400];
static char *LUMP_BUFFER;

static uint32_t WAD_LumpsAmount;
static uint32_t WAD_DirectoryAddress;
static lump_t *lumps; //array of lumps loaded from the Input Wad

static uint8_t FLAGS = 0;

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
static uint8_t isspace(char c) {
	return (c == 0x20 || c == 0x09 || (c >= 0x0a && c <= 0x0d));
}

// Get the value for a key in a block
static const char *getFieldValueFromBlock(const block_t *blk, const char *key) {
	for (uint8_t i = 0; i < blk->fieldsCount; i++) {
		if (!strcmp(blk->fields[i].key, key)) return blk->fields[i].value;
	}
	return 0;
}

// Check if the block contains a field with the given key
static uint8_t BOOL_BlockHasField(const block_t *blk, const char *strkey) {
	for (uint8_t i = 0; i < blk->fieldsCount; i++) {
		if (!strcmp(blk->fields[i].key, strkey)) return 1;
	}
	return 0;
}

//Add a key/value field into block
static void addField(block_t *blk, const char *key, const char *value) {
	field_t *tmp = (field_t*) realloc(blk->fields, (blk->fieldsCount + 1) * sizeof(field_t));
	if (!tmp) {
		fprintf(stderr, "Failed to reallocate memory for the new field in block\n");
		exit(1);
	}
	blk->fields = tmp;

	blk->fields[blk->fieldsCount].key = strdup(key);
	blk->fields[blk->fieldsCount].value = strdup(value);
	if (!blk->fields[blk->fieldsCount].key || !blk->fields[blk->fieldsCount].value) {
		fprintf(stderr, "addField: out of memory while copying strings\n");
		exit(1);
	}

	blk->fieldsCount++;
}

static void removeField(block_t *blk, const char *key) {
	for (uint8_t i = 0; i < blk->fieldsCount; i++) {
		if (!strcmp(blk->fields[i].key, key)) {
			free(blk->fields[i].key);
			//blk->fields[i].key = 0;
			free(blk->fields[i].value);
			//blk->fields[i].value = 0;
			memmove(&blk->fields[i], &blk->fields[i + 1], (blk->fieldsCount - i - 1) * sizeof(field_t));
			//for (uint8_t j = i; j < blk->fieldsCount - 1; j++) blk->fields[j] = blk->fields[j + 1];
			blk->fieldsCount--;

			if (blk->fieldsCount) {
				blk->fields = (field_t*) realloc(blk->fields, blk->fieldsCount * sizeof(field_t));
				if (!blk->fields) {
					fprintf(stderr, "Failed to reallocate memory for the reduced fields array in block\n");
					exit(1);
				}
			} else {
				free(blk->fields);
				blk->fields = 0;
			}
			return;
		}
	}
}

//Compare two block_t structs
static char BOOL_AreBlocksEqual(const block_t *a, const block_t *b) {
	if (a->fieldsCount != b->fieldsCount) return 0;
	char *matched = (char*) calloc(a->fieldsCount, 1);
	if (!matched) return 0;

	for (uint8_t i = 0; i < a->fieldsCount; i++) {
		int found = 0;
		for (uint8_t j = 0; j < b->fieldsCount; j++) {
			if (!matched[j] && !strcmp(a->fields[i].key, b->fields[j].key) && !strcmp(a->fields[i].value, b->fields[j].value)) {
				matched[j] = 1;
				found = 1;
				break;
			}
		}
		if (!found) { free(matched); return 0; }
	}
	free(matched);
	return 1;
}

//Parse the game config file
static char CONFIG_Parse(config_t *config) {
	if (!config) return 0;
	if (!config->json) return 0;

	config->linedefSpecialsNoTexture = 0;
	config->linedefSpecialsSlope = 0;
	config->sectorFieldsSlope = 0;
	config->thingTypesNoAngle = 0;
	for (uint8_t x = 0; x < 5; x++) config->defaultValues[x] = 0;

	json_value *j = config->json;

	for (uint16_t x = 0; x < j->u.object.length; x++) {
		if (!strncmp(j->u.object.values[x].name, "namespace", 9) && gameEngine != ENGINE_UNKNOWN) {
			//Cancel parsing if the config is made for another game

			if (strcmp(j->u.object.values[x].value->u.string.ptr, namespaceStr)) {
				fprintf(stderr, "ERROR: Config file is made for \"%s\", not for \"%s\" game engine\n", j->u.object.values[x].value->u.string.ptr, namespaceStr);
				return 0;
			}
		}

		//
		// LINEDEF
		//
		else if (!strncmp(j->u.object.values[x].name, "linedef", 7) && j->u.object.values[x].value->type == json_object) {

			for (uint16_t i = 0; i < j->u.object.values[x].value->u.object.length; i++) {
				bufferB = j->u.object.values[x].value->u.object.values[i].value->type;

				if (!strcmp(j->u.object.values[x].value->u.object.values[i].name, "specialsNoTexture") && bufferB == json_array) {
					//found array containing Linedef Special types that *do not* require sidefes textures

					bufferA = j->u.object.values[x].value->u.object.values[i].value->u.array.length;
					bufferA = (bufferA ? bufferA : 1);

					//Allocate memory
					config->linedefSpecialsNoTexture = (uint16_t *) malloc((bufferA + 1) * sizeof(uint16_t));
					if (!config->linedefSpecialsNoTexture) {
						fprintf(stderr, "ERROR: Failed to allocate memory for the non-textured Linedef Special types array");
						return 0;
					}

					//Copy data from JSON
					for (uint16_t a = 0; a < bufferA; a++) {
						config->linedefSpecialsNoTexture[a] = j->u.object.values[x].value->u.object.values[i].value->u.array.values[a]->u.integer;
					}

					config->linedefSpecialsNoTexture[bufferA] = 0;
				}

				else if (!strcmp(j->u.object.values[x].value->u.object.values[i].name, "specialsSlope") && bufferB == json_array) {
					//found array containing Linedef Special types that create slopes

					bufferA = j->u.object.values[x].value->u.object.values[i].value->u.array.length;
					bufferA = (bufferA ? bufferA : 1);

					//Allocate memory
					config->linedefSpecialsSlope = (uint16_t *) malloc((bufferA + 1) * sizeof(uint16_t));
					if (!config->linedefSpecialsSlope) {
						fprintf(stderr, "ERROR: Failed to allocate memory for the slope Linedef Special types array");
						return 0;
					}

					//Copy data from JSON
					for (uint16_t a = 0; a < bufferA; a++) {
						config->linedefSpecialsSlope[a] = j->u.object.values[x].value->u.object.values[i].value->u.array.values[a]->u.integer;
					}

					config->linedefSpecialsSlope[bufferA] = 0;
				}

				else if (!strcmp(j->u.object.values[x].value->u.object.values[i].name, "defaultValues") && bufferB == json_object) {
					//found array containing default field values for Linedefs

					bufferA = j->u.object.values[x].value->u.object.values[i].value->u.object.length;
					bufferA = (bufferA ? bufferA : 1);

					//Allocate memory
					config->defaultValues[LEVEL_LINEDEF] = (field_t *) malloc((bufferA + 1) * sizeof(field_t));
					if (!config->defaultValues[LEVEL_LINEDEF]) {
						fprintf(stderr, "ERROR: Failed to allocate memory for the Linedef default values list");
						return 0;
					}

					//Copy data from JSON
					for (uint16_t a = 0; a < bufferA; a++) {
						config->defaultValues[LEVEL_LINEDEF][a].key = strdup(j->u.object.values[x].value->u.object.values[i].value->u.object.values[a].name);
						config->defaultValues[LEVEL_LINEDEF][a].value = strdup(j->u.object.values[x].value->u.object.values[i].value->u.object.values[a].value->u.string.ptr);
					}

					config->defaultValues[LEVEL_LINEDEF][bufferA].key = 0;
					config->defaultValues[LEVEL_LINEDEF][bufferA].value = 0;
				}
			}
		}

		//
		// SIDEDEF
		//
		else if (!strncmp(j->u.object.values[x].name, "sidedef", 7) && j->u.object.values[x].value->type == json_object) {

			for (uint16_t i = 0; i < j->u.object.values[x].value->u.object.length; i++) {
				bufferB = j->u.object.values[x].value->u.object.values[i].value->type;

				if (!strcmp(j->u.object.values[x].value->u.object.values[i].name, "defaultValues") && bufferB == json_object) {
					//found array containing default field values for Sidedefs

					bufferA = j->u.object.values[x].value->u.object.values[i].value->u.object.length;
					bufferA = (bufferA ? bufferA : 1);

					//Allocate memory
					config->defaultValues[LEVEL_SIDEDEF] = (field_t *) malloc((bufferA + 1) * sizeof(field_t));
					if (!config->defaultValues[LEVEL_SIDEDEF]) {
						fprintf(stderr, "ERROR: Failed to allocate memory for the Sidedef default values list");
						return 0;
					}

					//Copy data from JSON
					for (uint16_t a = 0; a < bufferA; a++) {
						config->defaultValues[LEVEL_SIDEDEF][a].key = strdup(j->u.object.values[x].value->u.object.values[i].value->u.object.values[a].name);
						config->defaultValues[LEVEL_SIDEDEF][a].value = strdup(j->u.object.values[x].value->u.object.values[i].value->u.object.values[a].value->u.string.ptr);
					}

					config->defaultValues[LEVEL_SIDEDEF][bufferA].key = 0;
					config->defaultValues[LEVEL_SIDEDEF][bufferA].value = 0;
				}
			}
		}

		//
		// SECTOR
		//
		else if (!strncmp(j->u.object.values[x].name, "sector", 6) && j->u.object.values[x].value->type == json_object) {

			for (uint16_t i = 0; i < j->u.object.values[x].value->u.object.length; i++) {
				bufferB = j->u.object.values[x].value->u.object.values[i].value->type;

				if (!strcmp(j->u.object.values[x].value->u.object.values[i].name, "polygonSlope") && bufferB == json_boolean) {
					//Found information about polygonal slopes support

					if (j->u.object.values[x].value->u.object.values[i].value->u.boolean) config->flags |= CFGFLAG_POLYGONSLOPE;
					else config->flags &= ~CFGFLAG_POLYGONSLOPE;
				}

				else if (!strcmp(j->u.object.values[x].value->u.object.values[i].name, "fieldsSlope") && bufferB == json_array) {
					//found array containing sector UDMF fields that define slope

					bufferA = j->u.object.values[x].value->u.object.values[i].value->u.array.length;
					bufferA = (bufferA ? bufferA : 1);

					//Allocate memory for the array
					config->sectorFieldsSlope = (char**) malloc((bufferA + 1) * sizeof(char*));
					if (!config->sectorFieldsSlope) {
						fprintf(stderr, "ERROR: Failed to allocate memory for the slope Sector fields array");
						return 0;
					}

					//Copy data from JSON
					for (uint16_t a = 0; a < bufferA; a++) {
						//copy the string
						config->sectorFieldsSlope[a] = strdup(j->u.object.values[x].value->u.object.values[i].value->u.array.values[a]->u.string.ptr);
						if (!config->sectorFieldsSlope[a]) {
							fprintf(stderr, "ERROR: Failed to allocate memory for string (array index %d) in the slope Sector fields array", a);
							return 0;
						}
					}

					config->sectorFieldsSlope[bufferA] = 0;
				}

				else if (!strcmp(j->u.object.values[x].value->u.object.values[i].name, "defaultValues") && bufferB == json_object) {
					//found array containing default field values for Sectors

					bufferA = j->u.object.values[x].value->u.object.values[i].value->u.object.length;
					bufferA = (bufferA ? bufferA : 1);

					//Allocate memory
					config->defaultValues[LEVEL_SECTOR] = (field_t *) malloc((bufferA + 1) * sizeof(field_t));
					if (!config->defaultValues[LEVEL_SECTOR]) {
						fprintf(stderr, "ERROR: Failed to allocate memory for the Sector default values list");
						return 0;
					}

					//Copy data from JSON
					for (uint16_t a = 0; a < bufferA; a++) {
						config->defaultValues[LEVEL_SECTOR][a].key = strdup(j->u.object.values[x].value->u.object.values[i].value->u.object.values[a].name);
						config->defaultValues[LEVEL_SECTOR][a].value = strdup(j->u.object.values[x].value->u.object.values[i].value->u.object.values[a].value->u.string.ptr);
					}

					config->defaultValues[LEVEL_SECTOR][bufferA].key = 0;
					config->defaultValues[LEVEL_SECTOR][bufferA].value = 0;
				}
			}
		}

		//
		// THING
		//
		else if (!strncmp(j->u.object.values[x].name, "thing", 5) && j->u.object.values[x].value->type == json_object) {

			for (uint16_t i = 0; i < j->u.object.values[x].value->u.object.length; i++) {
				bufferB = j->u.object.values[x].value->u.object.values[i].value->type;

				if (!strcmp(j->u.object.values[x].value->u.object.values[i].name, "noAngle")  && bufferB == json_array) {
					//found array containing sector thing types that do not use angle

					bufferA = j->u.object.values[x].value->u.object.values[i].value->u.array.length;
					bufferA = (bufferA ? bufferA : 1);

					//Allocate memory for the array
					config->thingTypesNoAngle = (uint16_t*) malloc((bufferA + 1) * sizeof(uint16_t));
					if (!config->thingTypesNoAngle) {
						fprintf(stderr, "ERROR: Failed to allocate memory for the no angle Things array");
						return 0;
					}

					//Copy data from JSON
					for (uint16_t a = 0; a < bufferA; a++) {
						config->thingTypesNoAngle[a] = j->u.object.values[x].value->u.object.values[i].value->u.array.values[a]->u.integer;
					}

					config->thingTypesNoAngle[bufferA] = 0;
				}

				else if (!strcmp(j->u.object.values[x].value->u.object.values[i].name, "defaultValues") && bufferB == json_object) {
					//found array containing default field values for Things

					bufferA = j->u.object.values[x].value->u.object.values[i].value->u.object.length;
					bufferA = (bufferA ? bufferA : 1);

					//Allocate memory
					config->defaultValues[LEVEL_THING] = (field_t *) malloc((bufferA + 1) * sizeof(field_t));
					if (!config->defaultValues[LEVEL_THING]) {
						fprintf(stderr, "ERROR: Failed to allocate memory for the Thing default values list");
						return 0;
					}

					//Copy data from JSON
					for (uint16_t a = 0; a < bufferA; a++) {
						config->defaultValues[LEVEL_THING][a].key = strdup(j->u.object.values[x].value->u.object.values[i].value->u.object.values[a].name);
						config->defaultValues[LEVEL_THING][a].value = strdup(j->u.object.values[x].value->u.object.values[i].value->u.object.values[a].value->u.string.ptr);
					}

					config->defaultValues[LEVEL_THING][bufferA].key = 0;
					config->defaultValues[LEVEL_THING][bufferA].value = 0;
				}
			}
		}
	}

	return 1;
}

//Unload game config file
static void CONFIG_Free(config_t *config) {
	if (!config) return;

	if (config->json) { json_value_free(config->json); config->json = 0; }
	if (config->buffer) { free(config->buffer); config->buffer = 0; }
	if (config->linedefSpecialsNoTexture) { free(config->linedefSpecialsNoTexture); config->linedefSpecialsNoTexture = 0; }
	if (config->linedefSpecialsSlope) { free(config->linedefSpecialsSlope); config->linedefSpecialsSlope = 0; }
	if (config->thingTypesNoAngle) { free(config->thingTypesNoAngle); config->thingTypesNoAngle = 0; }

	if (config->sectorFieldsSlope) {
		for (uint16_t x = 0; config->sectorFieldsSlope[x]; x++) {
			free(config->sectorFieldsSlope[x]);
			config->sectorFieldsSlope[x] = 0;
		}
		free(config->sectorFieldsSlope);
		config->sectorFieldsSlope = 0;
	}

	for (uint16_t x = 0; x < 5; x++) {
		if (!config->defaultValues[x]) continue;
		for (uint16_t y = 0; config->defaultValues[x][y].key; y++) {
			if (config->defaultValues[x][y].key) { free(config->defaultValues[x][y].key); config->defaultValues[x][y].key = 0; }
			if (config->defaultValues[x][y].value) { free(config->defaultValues[x][y].value); config->defaultValues[x][y].value = 0; }
		}
		free(config->defaultValues[x]);
		config->defaultValues[x] = 0;
	}

	FLAGS &= ~FLAG_CONFIGLOADED;
}

// Given a linedef block, return an array of pointers to the sidedef blocks it references (sidefront and sideback).
// The returned array is always of size 2 (sidefront, sideback), with NULL for any missing side.
// The caller does not own the returned array (static buffer).
static block_t **LINEDEF_GetSidedefs(block_t *linedef, uint8_t *sidesCount) {
	block_t **sidedefs;
	sidedefs = (block_t**) malloc(sizeof(block_t*) * 2);
	*sidesCount = 0;
	const char *sidefront_str = getFieldValueFromBlock(linedef, "sidefront");
	const char *sideback_str  = getFieldValueFromBlock(linedef, "sideback");
	int32_t sidefront = -1, sideback = -1;
	if (sidefront_str) sidefront = strtol(sidefront_str, 0, 10);
	if (sideback_str)  sideback  = strtol(sideback_str,  0, 10);

	// Find the sidedef blocks by index
	uint8_t found = 0;
	int32_t idx = 0;
	for (uint32_t i = 0; i < blockCount; i++) {
		if (!strncmp(blocks[i].header, "sidedef", 7)) {
			if (idx == sidefront && sidefront >= 0) {
				sidedefs[0] = &blocks[i];
				found++;
			}
			if (idx == sideback && sideback >= 0) {
				sidedefs[1] = &blocks[i];
				found++;
			}
			idx++;
			if (found == 2) break;
		}
	}

	*sidesCount = (sidefront >= 0 && sideback >= 0 && sidefront != sideback) ? 2 : (sidefront >= 0 ? 1 : 0) + (sideback >= 0 && sideback != sidefront ? 1 : 0);
	return sidedefs;
}

// Collect linedef blocks that reference any sidedef belonging to the given sector.
// Returns a dynamically allocated array of block_t* and sets outCount. Caller must free the
// returned array (but not the block_t pointers themselves).
static block_t **SECTOR_GetLinedefs(uint32_t sectorIndex, uint32_t *outCount) {
	*outCount = 0;
	if (sectorIndex >= sectorCount) return 0;

	// First pass: find which sidedef indices point to this sector
	bufferA = 0; //side index
	bufferB = 0; //matching count
	int32_t *matchingSides = 0;
	snprintf(buffer_str, sizeof(buffer_str), "%u", sectorIndex);

	for (uint32_t i = 0; i < blockCount; i++) {
		if (!strncmp(blocks[i].header, "sidedef", 7)) {
			const char *sec = getFieldValueFromBlock(&blocks[i], "sector");
			if (sec) {
				uint32_t sval = strtol(sec, 0, 10);
				if (sval == sectorIndex) {
					matchingSides = (int32_t*) realloc(matchingSides, (bufferB + 1) * sizeof(int32_t));
					matchingSides[bufferB++] = bufferA;
				}
			}
			bufferA++;
		}
	}

	if (!bufferB) { //found no sidedefs referencing this sector
		free(matchingSides);
		return 0;
	}

	// Second pass: find linedefs that reference any of these sidedef indices
	block_t **found = 0;
	bufferA = 0; //found linedefs count
	for (uint32_t i = 0; i < blockCount; i++) {
		if (!strncmp(blocks[i].header, "linedef", 7)) {
			// examine each pair in the linedef; if any numeric value equals a matching sidedef index, record it
			for (uint8_t p = 0; p < blocks[i].fieldsCount; p++) {
				// check for sidedef references
				if (!strncmp(blocks[i].fields[p].key, "sidefront", 9) || !strncmp(blocks[i].fields[p].key, "sideback", 8)) {
					int32_t iv = strtol(blocks[i].fields[p].value, 0, 10);
					if (iv >= 0) {
						// compare against matchingSides
						for (uint16_t m = 0; m < bufferB; m++) {
							if (iv == matchingSides[m]) {
								found = (block_t**) realloc(found, (bufferA + 1) * sizeof(block_t*));
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
static block_t **SECTOR_GetPolygonVertices(uint32_t sectorIndex, uint32_t *outCount) {
	*outCount = 0;
	if (sectorIndex >= sectorCount) return 0;

	// First pass: collect sidedef indices belonging to this sector
	bufferA = 0; //side Idx
	bufferB = 0; //sidedef count
	int32_t *sidedefIndices = 0;
	for (uint32_t i = 0; i < blockCount; i++) {
		if (!strncmp(blocks[i].header, "sidedef", 7)) {
			const char *sec = getFieldValueFromBlock(&blocks[i], "sector");
			if (sec) {
				uint32_t sval = strtol(sec, 0, 10);
				if (sval == sectorIndex) {
					sidedefIndices = (int32_t*) realloc(sidedefIndices, (bufferB + 1) * sizeof(int32_t));
					sidedefIndices[bufferB++] = bufferA;
				}
			}
			bufferA++;
		}
	}
	if (!bufferB) { //found no sidedefs
		free(sidedefIndices);
		return 0;
	}

	// Build vertex list (ordered by occurrence) to index into
	bufferA = 0; //total vertices
	for (uint32_t i = 0; i < blockCount; i++) if (!strncmp(blocks[i].header, "vertex", 6)) bufferA++;
	if (!bufferA) { //
		free(sidedefIndices);
		return 0;
	}

	block_t **vertexList = (block_t**)malloc(bufferA * sizeof(block_t*));
	uint32_t vi = 0;
	for (uint32_t i = 0; i < blockCount; i++) if (!strncmp(blocks[i].header, "vertex", 6)) vertexList[vi++] = &blocks[i];

	// Second pass: for every linedef, if it references any of the sidedefIndices, collect its v1/v2
	block_t **found = 0;
	uint32_t foundCount = 0;
	for (uint32_t i = 0; i < blockCount; i++) {
		if (!strncmp(blocks[i].header, "linedef", 7)) {
			// check fields for sidefront/sideback
			char referencesSector = 0;
			for (uint16_t p = 0; p < blocks[i].fieldsCount; ++p) {
				if (!strncmp(blocks[i].fields[p].key, "sidefront", 9) || !strncmp(blocks[i].fields[p].key, "sideback", 8)) {
					int32_t iv = strtol(blocks[i].fields[p].value, 0, 10);
					if (iv >= 0) {
						for (uint32_t m = 0; m < bufferB; m++) {
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
				uint32_t idx = strtol(vs, 0, 10);
				if (idx < bufferA) {
					// avoid duplicates
					char already = 0;
					for (uint32_t f = 0; f < foundCount; f++) if (found[f] == vertexList[idx]) { already = 1; break; }
					if (!already) {
						found = (block_t**) realloc(found, (foundCount + 1) * sizeof(block_t*));
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
static char BOOL_IsSectorSloped(uint32_t sectorIndex) {
	if (sectorIndex >= sectorCount) return 0;
	block_t *sectorBlk = sectors[sectorIndex].block;

	if (config.sectorFieldsSlope) {
		for (uint16_t x = 0; config.sectorFieldsSlope[x]; x++) {
			if (BOOL_BlockHasField(sectorBlk, config.sectorFieldsSlope[x])) return 1;
		}
	}

	// Collect linedefs associated with this sector (via sidedefs)
	bufferA = 0; //linedef count
	block_t **linedefs = SECTOR_GetLinedefs(sectorIndex, &bufferA);
	if (!linedefs) return 0; // no linedefs referencing sidedefs of this sector

	// Inspect collected linedefs for slope-creating properties (conservative)
	if (config.linedefSpecialsSlope) {
		for (uint32_t i = 0; i < bufferA; i++) {
			const char *specs = getFieldValueFromBlock(linedefs[i], "special");
			bufferB = strtol(specs ? specs : "0", 0, 10); //Linedef special number

			for (uint16_t s = 0; config.linedefSpecialsSlope[s]; s++) {
				if (bufferB == config.linedefSpecialsSlope[s]) {
					//Found a slope creating linedef - sector is sloped
					free(linedefs);
					return 1;
				}
			}
		}
	}

	if (gameEngine == ENGINE_SRB2) {
		// Polygon-only vertex check: collect unique polygon vertices for the sector and inspect them.
		// Stricter rule: require at least two distinct numeric vertex heights to mark the sector sloped.
		bufferA = 0; //polyVertexCount
		block_t **polyVertices = SECTOR_GetPolygonVertices(sectorIndex, &bufferA);

		if (polyVertices) {
			double *zvals = 0;
			uint32_t zcount = 0;

			for (uint32_t pv = 0; pv < bufferA; pv++) {

				const char *zstr = 0;
				if (BOOL_BlockHasField(polyVertices[pv], "zfloor")) zstr = getFieldValueFromBlock(polyVertices[pv], "zfloor");
				else if (BOOL_BlockHasField(polyVertices[pv], "zceiling")) zstr = getFieldValueFromBlock(polyVertices[pv], "zceiling");

				if (zstr && *zstr) {
					double zv = strtod(zstr, 0);
					char found = 0;

					for (uint32_t zi = 0; zi < zcount; zi++)
						if (zvals && zvals[zi] == zv) { found = 1; break; }

					if (!found) {
						zvals = (double*) realloc(zvals, (zcount + 1) * sizeof(double));
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

static void MAP_RemoveControlLineTextures() {
	printf("Removing textures on control linedefs that do not require them... ");
	block_t *b;

	for (uint32_t x = 0; x < blockCount; x++) {
		b = &blocks[x];

		if (!strncmp(b->header, "linedef", 7)) {
			for (uint8_t i = 0; i < b->fieldsCount; i++) {
				if (!strncmp(b->fields[i].key, "special", 7)) {
					for (uint16_t a = 0; config.linedefSpecialsNoTexture[a]; a++) {
						if (strtol(b->fields[i].value, 0, 10) == config.linedefSpecialsNoTexture[a]) {
							uint8_t numSides;
							block_t **sidedefs = LINEDEF_GetSidedefs(b, &numSides);
							for (uint8_t side = 0; side < numSides; side++) {
								removeField(sidedefs[side], "texturetop");
								removeField(sidedefs[side], "texturemiddle");
								removeField(sidedefs[side], "texturebottom");
							}
							free(sidedefs);
							sidedefs = 0;
							break;
						}
					}
				}
			}
		}
	}
	puts("Done");
}

static void MAP_MergeSectors() {
	printf("Merging the identical sectors... ");

	sectorCount = 0;
	//Count the amount of sectors in map
	for (uint32_t i = 0; i < blockCount; i++) {
		if (!strncmp(blocks[i].header, "sector", 6)) sectorCount++;
	}
	sectors = (sector_t*)calloc(sectorCount, sizeof(sector_t)); //Allocate the space for sectors
	//Load sectors
	bufferA = 0; //Sector counter
	for (uint32_t i = 0; i < blockCount; i++) {
		if (!strncmp(blocks[i].header, "sector", 6)) sectors[bufferA++].block = &blocks[i];
	}
	// Mark all sectors as unvisited by the program
	for (uint32_t i = 0; i < sectorCount; i++) sectors[i].isMaster = -1;

	// Precompute slope flags for all sectors so we don't merge sloped sectors
	for (uint32_t si = 0; si < sectorCount; si++) sectors[si].isSlope = 0;
	for (uint32_t si = 0; si < sectorCount; si++) sectors[si].isSlope = BOOL_IsSectorSloped(si);

	//Find identical sectors and mark them
	uint32_t uniqueSectorID = 0;
	for (uint32_t i = 0; i < sectorCount; i++) {
		if (sectors[i].isMaster != -1) continue; //Skip sector because it is already processed

		sectors[i].isMaster = 1;
		sectors[i].masterID = uniqueSectorID;
		sectors[i].sectorID = i;

		//Compare with other sectors
		for (uint32_t j = i + 1; j < sectorCount; j++) {
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
	uint32_t sidedefCount = 0;
	for (uint32_t i = 0; i < blockCount; i++) {
		if (!strncmp(blocks[i].header, "sidedef", 7)) sidedefCount++;
	}
	//Load Sidedefs
	sidedefs = (block_t*)malloc(sizeof(block_t) * sidedefCount); //Allocate space for sidedefs
	bufferA = 0; //Sidedef counter
	for (uint32_t i = 0; i < blockCount; i++) {
		if (!strncmp(blocks[i].header, "sidedef", 7)) sidedefs[bufferA++] = blocks[i];
	}

	// Build masterID -> new compacted index
	int *masterID_to_newIndex = (int*)malloc(uniqueSectorID * sizeof(int));
	bufferA = 0; //new index counter
	for (uint32_t i = 0; i < sectorCount; i++) {
		if (sectors[i].isMaster == 1) masterID_to_newIndex[sectors[i].masterID] = bufferA++;
	}
	int *oldToNew = (int*) malloc(sectorCount * sizeof(int));
	for (uint32_t i = 0; i < sectorCount; i++) oldToNew[i] = masterID_to_newIndex[sectors[i].masterID];
	free(masterID_to_newIndex);
	masterID_to_newIndex = 0;
	
	// Remap sidedef sector indices
	for (uint32_t i = 0; i < sidedefCount; i++) {
		for (uint16_t j = 0; j < sidedefs[i].fieldsCount; j++) {
			if (!strncmp(sidedefs[i].fields[j].key, "sector", 6)) {
				uint32_t sectorIndex = strtol(sidedefs[i].fields[j].value, 0, 10);
				
				if (sectorIndex < sectorCount) {
					snprintf(buffer_str, sizeof(buffer_str), "%d", oldToNew[sectorIndex]);
					char *old = sidedefs[i].fields[j].value;
					sidedefs[i].fields[j].value = strdup(buffer_str);
					free(old); old = 0;
				} else {
					fprintf(stderr, "WARNING: Invalid or out-of-bounds sector index '%s' for sidedef, setting to 0\n", sidedefs[i].fields[j].value);
					strcpy(sidedefs[i].fields[j].value, "0");
				}
			}
		}
	}
	free(oldToNew);

	//Remove sector duplicates
	uint32_t writeIndex = 0;
	bufferA = 0; //track which sector we're looking at in sector[]

	for (uint32_t i = 0; i < blockCount; i++) {
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
				for (uint16_t j = 0; j < blocks[i].fieldsCount; j++) {
					free(blocks[i].fields[j].key);
					blocks[i].fields[j].key = 0;
					free(blocks[i].fields[j].value);
					blocks[i].fields[j].value = 0;
				}
				free(blocks[i].fields);
				blocks[i].fields = 0;
			}
			bufferA++;
		}
	}

	blockCount = writeIndex;

	printf("Done (before: %d, after: %d)\n", sectorCount, uniqueSectorID);

	free(sectors);
	free(sidedefs);
}

//Make things that do not use angles face East (angle 0)
static void MAP_NoAngleThings() {
	printf("Adjusting no-angle Things to face East... ");
	block_t *b;
	bufferA = 0; //thing count

	for (uint32_t x = 0; x < blockCount; x++) {
		b = &blocks[x];

		if (!strncmp(b->header, "thing", 5)) {
			for (uint8_t i = 0; i < b->fieldsCount; i++) {
				if (!strncmp(b->fields[i].key, "type", 7)) {
					for (uint16_t a = 0; config.thingTypesNoAngle[a]; a++) {
						if (strtol(b->fields[i].value, 0, 10) == config.thingTypesNoAngle[a]) {
							removeField(b, "angle");
							bufferA++;
							break;
						}
					}
				}
			}
		}
	}

	printf("Done (%u things)\n", bufferA);
}

//Remove UDMF fields that match the default values
static void MAP_RemoveDefaultValues() {
	printf("Removing UDMF fields that match the default values... ");
	uint8_t levelElement;
	for (uint32_t b = 0; b < blockCount; b++) { // for each block
		if (!strncmp(blocks[b].header, "linedef", 7)) levelElement = LEVEL_LINEDEF;
		else if (!strncmp(blocks[b].header, "sidedef", 7)) levelElement = LEVEL_SIDEDEF;
		else if (!strncmp(blocks[b].header, "sector", 6)) levelElement = LEVEL_SECTOR;
		else if (!strncmp(blocks[b].header, "thing", 5)) levelElement = LEVEL_THING;
		else continue;

		uint8_t y = 0;
		while (y < blocks[b].fieldsCount) {
			int match = 0;
			for (uint16_t x = 0; config.defaultValues[levelElement][x].key; x++) {
				if (!strcmp(config.defaultValues[levelElement][x].key, blocks[b].fields[y].key) && !strcmp(config.defaultValues[levelElement][x].value, blocks[b].fields[y].value)) {
					removeField(&blocks[b], blocks[b].fields[y].key);
					match = 1;
					break; // restart at same y, as fields have shifted
				}
			}
			if (!match) y++;
		}
	}
	puts("Done");
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
		if (isspace(*ptr)) { ptr++; continue; }

		// namespace
		if (!strncmp(ptr, "namespace", 9)) {
			ptr += 9;
			while (isspace(*ptr)) ptr++;
			if (*ptr == '=') ptr++;
			while (isspace(*ptr)) ptr++;
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
			if (FLAGS & FLAGS_CUSTOMCONFIG) {
				gameEngine = ENGINE_UNKNOWN;
			} else {
				if (namespaceStr) {
					if (!strncmp(namespaceStr, "doom", 4)) gameEngine = ENGINE_DOOM;
					else if (!strncmp(namespaceStr, "heretic", 7)) gameEngine = ENGINE_HERETIC;
					else if (!strncmp(namespaceStr, "hexen", 5)) gameEngine = ENGINE_HEXEN;
					else if (!strncmp(namespaceStr, "strife", 6)) gameEngine = ENGINE_STRIFE;
					else if (!strncmp(namespaceStr, "zdoom", 5)) gameEngine = ENGINE_ZDOOM;
					else if (!strncmp(namespaceStr, "srb2", 4)) gameEngine = ENGINE_SRB2;
					else gameEngine = ENGINE_UNKNOWN;
				} else {
					gameEngine = ENGINE_UNKNOWN;
				}
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
			if ((*ptr == '/' && (ptr[1] == '/' || ptr[1] == '*')) || isspace(*ptr)) break; // don't include comment start or whitespace in header
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
			if (isspace(*ptr)) { ptr++; continue; }
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
			if (isspace(*ptr)) { ptr++; continue; }
			if (*ptr == '}') { ptr++; break; }

			// read key
			char key[128]; int ki = 0;
			while (*ptr && *ptr != '=' && *ptr != '}' && !isspace(*ptr)) {
				if (ki < (int)sizeof(key)-1) key[ki++] = *ptr;
				ptr++;
			}
			key[ki] = '\0';
			while (isspace(*ptr)) ptr++;
			if (*ptr == '=') ptr++;
			while (isspace(*ptr)) ptr++;

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
static char* TEXTMAP_Generate(block_t *blocks) {
	uint32_t allocated = 0x100000; //1 Megabyte
	uint32_t used = 0;
	char *out = (char*)malloc(allocated);
	if (!out) {
		fprintf(stderr, "Failed to allocate memory for the new TEXTMAP lump\n");
		return 0;
	}

	used += snprintf(out, allocated, "namespace=\"%s\";", namespaceStr);

	for (uint32_t b = 0; b < blockCount; b++) {
		//Calculate the character length of the block we are about to write
		uint32_t block_len = strlen(blocks[b].header) + 2;
		for (uint8_t p = 0; p < blocks[b].fieldsCount; p++) {
			block_len += strlen(blocks[b].fields[p].key) + strlen(blocks[b].fields[p].value) + 2;
		}
		
		//If the buffer is going to be bigger than the amout of space we allocated, allocate more memory
		while (used + block_len + 1 > allocated) {
			allocated *= 2;
			out = (char*) realloc(out, allocated);
			if (!out) {
				fprintf(stderr, "Failed to reallocate memory for the new TEXTMAP lump (%u bytes)\n", allocated);
				return 0;
			}
		}

		//Write the data itself
		used += snprintf(out + used, allocated - used, "%s{", blocks[b].header);
		for (uint8_t p = 0; p < blocks[b].fieldsCount; p++) {
			used += snprintf(out + used, allocated - used, "%s=%s;", blocks[b].fields[p].key, blocks[b].fields[p].value);
		}
		used += snprintf(out + used, allocated - used, "}");
	}

	//Final newline
	if (used + 1 > allocated) {
        allocated += 2;
        out = (char*) realloc(out, allocated);
		if (!out) {
			fprintf(stderr, "Failed to reallocate memory for the new TEXTMAP lump (%u bytes)\n", allocated);
			return 0;
		}
    }
    out[used++] = '\n';
    out[used] = 0;
	return out;
}


//
// MAIN
//
int main(int argc, char *argv[]) {
	puts("LESSUDMF v4.0 by LeonardoTheMutant\n");

	if (argc < 2) {
		printf("%s <input.wad> [-o <output.wad>] ...\n", argv[0]);
		puts("Optimize the UDMF maps data in WAD\n");
		printf("    -o <output.wad>\tOutput to the file. If not given, the output will be written to %s\n", outputFilePath);
		puts("    -c <config.json>\tLoad custom game engine configuration");
		puts("    -t\t\tPreserve textures on control linedefs with line specials that do not use them");
		puts("    -s\t\tPreserve information about identical sectors, do not merge them with each other");
		puts("    -a\t\tPreserve angle facing information for things that are no-angle");
		puts("    -f\t\tPreserve the UDMF fields which are set to default values");
		puts("\nAlways make sure to have a copy of the old file - new file can have corruptions!");
		return 0;
	}

	// parse args
	for (int i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "-o", 2) && i + 1 < argc) strncpy(outputFilePath, argv[++i], sizeof(outputFilePath) - 1); //output file
		else if (!strncmp(argv[i], "-c", 2) && i + 1 < argc) { //custom game configuration
			i++;
			configFiles[ENGINE_UNKNOWN] = strdup(argv[i]);
			if (!configFiles[ENGINE_UNKNOWN]) fprintf(stderr, "Failed to allocate memory for the Config File (%d bytes)\n", config.filesize);
			FLAGS |= FLAGS_CUSTOMCONFIG; //"Using custom config file"
		}
		else if (!strncmp(argv[i], "-t", 2)) FLAGS |= FLAG_PRESERVETEXTURES; //"No texture removal"
		else if (!strncmp(argv[i], "-s", 2)) FLAGS |= FLAG_PRESERVESECTORS; //"No sector merging"
		else if (!strncmp(argv[i], "-a", 2)) FLAGS |= FLAG_PRESERVEANGLES; //"No angle 0 things"
		else if (!strncmp(argv[i], "-f", 2)) FLAGS |= FLAG_PRESERVEDEFAULT; //"Keep default values"
		else strncpy(buffer_str, argv[i], sizeof(buffer_str));
	}

	if (BOOL_AreSameFiles(buffer_str, outputFilePath)) { fprintf(stderr, "ERROR: Input and Output files are the same, please choose other Output file\n"); return 1; }

	inputWAD = fopen(buffer_str, "rb");
	if (!inputWAD) { fprintf(stderr, "ERROR: Failed to open Input WAD (%s)\n", buffer_str); return 1; }
	outputWAD = fopen(outputFilePath, "wb");
	if (!outputWAD) { fprintf(stderr, "ERROR: Failed to open Output WAD (%s)\n", outputFilePath); closeIO(); return 1; }

	memset(buffer_str, 0, sizeof(buffer_str));

	//Read the WAD type
	fread(buffer_str, 1, 4, inputWAD);
	if (strncmp(buffer_str + 1, "WAD", 3)) { fprintf(stderr, "ERROR: Bad Input WAD header\n"); closeIO(); return 1; }
	fwrite(buffer_str, 1, 4, outputWAD);

	//Get the amount of lumps in WAD and allocate the space for them
	fread(&WAD_LumpsAmount, 4, 1, inputWAD);
	fwrite(&WAD_LumpsAmount, 4, 1, outputWAD);
	lumps = (lump_t*) malloc(sizeof(lump_t) * WAD_LumpsAmount);
	if (!lumps) { fprintf(stderr, "ERROR: Failed to allocate memory for the WAD lumps buffer"); closeIO(); return 1; }

	//Get Directory Table address
	fread(&WAD_DirectoryAddress, 4, 1, inputWAD);

	// Prepare to write new WAD: write placeholder for the Directory Table address (we'll correct it at the end)
	putc(0x0C, outputWAD);
	for (uint8_t x=0; x<3; x++) putc(0, outputWAD);

	// Seek to dir table, read it
	fseek(inputWAD, WAD_DirectoryAddress, SEEK_SET);
	puts("Directory Table of the Input WAD:");
	puts("\nID   ADRESS     SIZE     NAME");
	for (uint16_t i = 0; i < WAD_LumpsAmount; i++) {
		fread(&lumps[i].adress, 4, 1, inputWAD);
		fread(&lumps[i].size, 4, 1, inputWAD);
		fread(lumps[i].name, 8, 1, inputWAD);
		printf("%2d %8d %8d %8s\n", i, lumps[i].adress, lumps[i].size, lumps[i].name);
	}

	// Copy/modify lumps
	fseek(inputWAD, 0x0C, SEEK_SET); //Jump back to the actuall lump data
	for (uint16_t i = 0; i < WAD_LumpsAmount; i++) {
		// Set new lump address
		lumps[i].adress = ftell(outputWAD);
		if (strncmp(lumps[i].name, "TEXTMAP", 7)) {
			//Lump is not TEXTMAP, copy the lump contents to the Output WAD unmodified
			if (lumps[i].size > 0) {
				LUMP_BUFFER = (char*) malloc(lumps[i].size);
				fread(LUMP_BUFFER, lumps[i].size, 1, inputWAD);
				fwrite(LUMP_BUFFER, lumps[i].size, 1, outputWAD);
				free(LUMP_BUFFER);
			}
		} else {
			//---------- Modify TEXTMAP ----------
			printf("\n* Working on TEXTMAP of %s *\n", lumps[i-1].name);

			//Copy TEXTMAP to memory
			LUMP_BUFFER = (char*)malloc(lumps[i].size + 1);
			fread(LUMP_BUFFER, lumps[i].size, 1, inputWAD);
			LUMP_BUFFER[lumps[i].size] = '\0';

			// Free old blocks if any
			for (uint32_t b = 0; b < blockCount; b++) {
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

			//Load the configuration file for the specified game engine so the program knows better what to optimize
			if (gameEngine == gameEngine_last) goto skip_config_load;
			if (stat(configFiles[gameEngine], &config.filestatus)) {
				fprintf(stderr, "Config file %s not found, \n", configFiles[gameEngine]);
			} else {
				if (FLAGS & FLAG_CONFIGLOADED) CONFIG_Free(&config);
				if (configFiles[ENGINE_UNKNOWN]) puts("! Using custom game configuration !");
				config.flags = 0;

				//Try to load the file itself
				config.filesize = config.filestatus.st_size;

				config.buffer = (char*) malloc(config.filesize + 1);
				if (!config.buffer) {
					fprintf(stderr, "Failed to allocate memory for the Config File (%u bytes)\n", config.filesize + 1);
					goto skip_config_load;
				}
				
				configFile = fopen(configFiles[gameEngine], "rb");
				if (!configFile) {
					fprintf(stderr, "Failed to open the %s config file\n", configFiles[gameEngine]);
					free(config.buffer);
					config.buffer = 0;
					goto skip_config_load;
				}

				fread(config.buffer, config.filesize, 1, configFile);
				config.buffer[config.filesize] = 0;
				fclose(configFile);


				config.json = json_parse(config.buffer, config.filesize);
				if (!config.json) {
					fprintf(stderr, "Failed to parse JSON data from the config file\n");
					free(config.buffer);
					config.buffer = 0;
					goto skip_config_load;
				}

				if (!CONFIG_Parse(&config)) {
					fprintf(stderr, "Failed to parse the config file, program will not do deep level optimization\n");
					CONFIG_Free(&config);
					memset(&config, 0, sizeof(config));
					goto skip_config_load;
				}

				FLAGS |= FLAG_CONFIGLOADED;
			}

			skip_config_load:

			if (FLAGS & FLAG_CONFIGLOADED) {
				//Remove unrequired textures from control linedefs (can be disabled with "-t" CLI option)
				if (!(FLAGS & FLAG_PRESERVETEXTURES)) MAP_RemoveControlLineTextures();

				// Merge identical sectors (can be disabled with "-s" CLI option)
				if (!(FLAGS & FLAG_PRESERVESECTORS)) MAP_MergeSectors();

				//Force some things to face East (angle 0) (can be disabled with "-a" CLI option)
				if (!(FLAGS & FLAG_PRESERVEANGLES)) MAP_NoAngleThings();

				//Remove UDMF fields which are set to default value (can be disabled with "-f" CLI option)
				if (!(FLAGS & FLAG_PRESERVEDEFAULT)) MAP_RemoveDefaultValues();
			}

			LUMP_BUFFER = TEXTMAP_Generate(blocks); //Write new lump to the buffer
			lumps[i].size = strlen(LUMP_BUFFER);

			// Write the new TEXTMAP to the Output WAD
			fwrite(LUMP_BUFFER, lumps[i].size, 1, outputWAD);
			printf("* Wrote the modified TEXTMAP data of %s to the output *\n", lumps[i-1].name);

			free(LUMP_BUFFER);
			gameEngine_last = gameEngine;
		}
	}

	if (FLAGS & FLAG_CONFIGLOADED) CONFIG_Free(&config);

	// Directory Table address
	bufferA = ftell(outputWAD); //where we are in the output WAD
	fseek(outputWAD, 8, SEEK_SET); //go back to the Directory Table address pointer in WAD
	fwrite(&bufferA, 4, 1, outputWAD); //write the address
	fseek(outputWAD, bufferA, SEEK_SET); //get back to the Directory Table

	//Write the new Directory Table
	puts("\nDirectory Table of the Output WAD:");
	puts("\nID   ADRESS     SIZE     NAME");
	for (uint16_t i = 0; i < WAD_LumpsAmount; i++) {
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
