# LESSUDMF
A tool to optimize UDMF maps data in WAD files

## Capabilities
- Cleanup the TEXTMAP lump inside WAD from comments, whitespaces and newlines
- Merge identical sectors in maps so the sector duplicates are removed

## How to use
Run `LESSUMDF [Inputfile.WAD]` from terminal and you get your WAD back with optimized UDMF maps. Just replace the `[Inputfile.WAD]` with a real file path to the WAD file. The WAD file size becomes smaller by 30% on average. **Always make sure you have a recovery copy of your Input WAD and check the contents of the Output WAD!**

## Command line parameters
- `-o [Outputfile.WAD]` - Output to the file. If not given, an `./OUTPUT.WAD` file will be created instead.
- `-nm` - Do not merge the identical sectors in maps and remove sector duplicates from TEXTMAP.

## Compiling
Simply compile the source code file (`gcc LESSUDMF.C`) and the program is ready to be used. Tested with `gcc` and `tcc` compilers on Windows and Linux. Additional compile flags like `-O2` and `-std=c99` may also be allpied.

## Game engine compatibility
| Game | Support |
| --- | --- |
| Doom | Partial |
| Heretic | Partial |
| Hexen | Partial |
| Strife | Partial |
| ZDoom | Partial |
| Sonic Robo Blast 2 | Full |

***Maps for partially supported or unknown game engines may have the sloped sectors get merged with each other! Use with caution or use the `-nm` command line parameter!***