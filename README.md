# LESSUDMF
A tool to optimize UDMF map data in WAD files

## Capabilities
- Cleanup the TEXTMAP lump from comments, whitespaces and newlines
- Merge identical sectors in maps so the sector duplicates are removed

## How to use
Simply run `LESSUMDF [Inputfile.WAD]` from terminal and your get your WAD back with optimized UDMF maps. Just replace the `[Inputfile.WAD]` with a real file path to the WAD file. The WAD file size becomes smaller by 30% on average. **Always make sure you have a copy of your Input WAD and check the contents of the Output WAD!**

## Command line parameters
- `-o [Outputfile.WAD]` - Output to the file. If not given, an `./OUTPUT.WAD` file will be created instead.
- `-m` - Merge identical sectors in map and remove sector duplicates from TEXTMAP. **Use this with caution as it may merge sloped sectors together and eventually break them!**

## Compiling
Simply compile the source code file (`gcc LESSUDMF.C`) and the program is ready to be used. Tested with `gcc` and `tcc` on Windows and Linux. Additional compile flags like `-O2` and `-std=c99` may also be allpied.

## Disclamer
This program was tested only on Sonic Robo Blast 2 map WADs made with ZNODES node builder.
