# LESSUDMF
A tool to optimize UDMF maps data in WAD files

## Capabilities
- Cleanup the TEXTMAP lump inside WAD from comments, whitespaces and newlines
- Remove textures from control linedefs with special types that do not require textures to function
- Remove wall textures from sides that are not visible because of the sector heights difference
- Merge identical sectors in maps so the sector duplicates are removed
- Make no-angle things face East (and not use the `angle` field)
- Remove UDMF fields from TEXTMAP which are set to default values

## Disclamer
***This tool is not perfect. It may mess with the level data (geometry, textures, etc.) it is not supposed to optimize or ignore things that are definitely meant to be optimized/cleaned-up. I highly recommend having a backup copy of your map that you can always return to in case the tool messes up. I am trying my best to make the tool stable & reliable for all uses.***

## How to use
Run `LESSUMDF <input.wad>` from the terminal or drag&drop the WAD file onto the executable (in Windows) and get your WAD back with optimized UDMF maps inside. The WAD file size becomes smaller by 33% on average. **Always make sure you have a recovery copy of your Input WAD and check the contents of the Output WAD!**

## Command line parameters
- `-o <file.wad>` - Output to the file. If not given, an `./OUTPUT.WAD` file will be created instead.
- `-c <Config.json>` - Load a custom game engine configuration file, instead of the default ones (depending on the detected game engine for map)
- `-t` - Preserve textures on walls that do not require them.
- `-s` - Do not merge the identical sectors in maps and remove sector duplicates.
- `-a` - Do not force things that are no-angle to face East (angle 0)
- `-f` - Do not remove UDMF fields which are set to default values from TEXTMAP

## Compiling
Simply compile the source code file using `make` and the program is ready to be used. Tested with `gcc` and `tcc` compilers on Windows and Linux. Additional compile optimization flags like `-O2` may also be allpied.

## Game engine compatibility
UDMF is meant to be universal, so is this tool. You can throw WAD files with any levels for any game and the map data will get optimized.

***As of now, the additional optimization steps are only available to Sonic Robo Blast 2 maps due to the lack of config files for other engines.***
Full support for more game engines *may* to be added in the future