#include <libretro.h>
#include "pcsx2/Patch.h"

extern retro_environment_t environ_cb;

void lrps2_ingame_patches(const char *serial, const char *renderer,
		bool nointerlacing_hint,
		bool disable_mipmaps,
		bool game_enhancements,
		int8_t hint_widescreen,
		int8_t uncapped_framerate)
{
	struct retro_variable var;

	if (nointerlacing_hint)
	{
		if (!strncmp("SLUS-", serial, strlen("SLUS-")))
		{
			/* Ace Combat 04 - Shattered Skies (NTSC-U) [CRC: A32F7CD0] */
			if (!strcmp(serial, "SLUS-20152"))
			{
				/* Patch courtesy: asasega */
				int i;
				char *patches[] = {
					"patch=1,EE,E0050003,extended,0029F418",
					"patch=1,EE,0029F418,extended,00000001",
					"patch=1,EE,D029F420,extended,0000948C",
					"patch=1,EE,0029F420,extended,00000000",
					"patch=1,EE,D029F420,extended,00009070",
					"patch=1,EE,0029F420,extended,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Ace Combat 5 - The Unsung War (NTSC-U) [CRC: 39B574F0] */
			else if (!strcmp(serial, "SLUS-20851"))
			{
				/* Patch courtesy: asasega */
				int i;
				char *patches[] = {
					"patch=1,EE,2032CA34,extended,0C03FFF3",
					"patch=1,EE,2032CA3C,extended,00000000",
					"patch=1,EE,200FFFCC,extended,341B0001",
					"patch=1,EE,200FFFD0,extended,147B0004",
					"patch=1,EE,200FFFD4,extended,34030001",
					"patch=1,EE,200FFFD8,extended,FC430000",
					"patch=1,EE,200FFFDC,extended,03E00008",
					"patch=1,EE,200FFFE0,extended,DE020010",
					"patch=1,EE,200FFFE4,extended,FC430000",
					"patch=1,EE,200FFFE8,extended,DE020010",
					"patch=1,EE,200FFFEC,extended,03E00008",
					"patch=1,EE,200FFFF0,extended,30429400"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Ace Combat Zero - The Belkan War (NTSC-U) [CRC: 65729657] */
			else if (!strcmp(serial, "SLUS-21346"))
			{
				/* Patch courtesy: asasega */
				int i;
				char *patches[] = {
					"patch=1,EE,202F9A14,extended,24020001",
					"patch=1,EE,202F9D58,extended,0C03FFF0",
					"patch=1,EE,200FFFC0,extended,341B9070",
					"patch=1,EE,200FFFC4,extended,145B0002",
					"patch=1,EE,200FFFCC,extended,34029000",
					"patch=1,EE,200FFFD0,extended,FCC20000",
					"patch=1,EE,200FFFD4,extended,03E00008"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Ape Escape 2 (NTSC-U) [CRC: BDD9F5E1] */
			else if (!strcmp(serial, "SLUS-20685"))
			{
				/* Patch courtesy: NineKain */
				int i;
				char *patches[] = {
					"patch=1,EE,00155580,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Burnout Revenge (NTSC-U) [CRC: D224D348] */
			else if (!strcmp(serial, "SLUS-21242"))
			{
				int i;
				char *patches[] = {
					/* Always ask for progressive scan */
					"patch=0,EE,2019778C,extended,10A2001C"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Drakengard (NTSC-U) [CRC: 9679D44C] */
			else if (!strcmp(serial, "SLUS-20732"))
			{
				/* TODO/FIXME - screen cutoff a little on the bottom */
				int i;
				char *patches[] = {
					/* NOP interlacing */
					"patch=1,EE,204F2668,extended,00000050",
					"patch=1,EE,204F2674,extended,000001E0",
					"patch=1,EE,204F2684,extended,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Drakengard 2 (NTSC-U) [CRC: 1648E3C9] */
			else if (!strcmp(serial, "SLUS-21373"))
			{
				/* Patch courtesy: umechan */
				/* TODO/FIXME - screen cutoff a little on the bottom */
				int i;
				char *patches[] = {
					"patch=1,EE,E0030003,extended,00456DA0",
					"patch=1,EE,20456DA0,extended,00000001",
					"patch=1,EE,20456DB0,extended,00001450",
					"patch=1,EE,20456DBC,extended,001DF9FF",
					"patch=1,EE,E0029400,extended,00456DB0",
					"patch=1,EE,20456DB0,extended,0000948C",
					"patch=1,EE,20456DBC,extended,001DF9FF",
					"patch=1,EE,E0030001,extended,00456D54",
					"patch=1,EE,20456D38,extended,00000050",
					"patch=1,EE,20456D44,extended,000001E1",
					"patch=1,EE,20456D54,extended,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Enthusia - Professional Racing (NTSC-U) [CRC: 81D233DC] */
			else if (!strcmp(serial, "SLUS-20967")) 
			{
				int i;
				char *patches[] = {
					"patch=1,EE,2013363C,word,34060001",
					"patch=1,EE,20383A40,word,00009450"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Eternal Poison (NTSC-U) [CRC: 2BE55519] */
			else if (!strcmp(serial, "SLUS-21779"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,0032DC7C,word,00000000",
					"patch=1,EE,0032DD04,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* God Hand (NTSC-U) [CRC: 6FB69282] */
			else if (!strcmp(serial, "SLUS-21503"))
			{
				int i;
				char *patches[] = {
					"patch=0,EE,002BE190,extended,24050000",
					"patch=0,EE,002BE194,extended,24060050",
					"patch=0,EE,2030CD10,extended,240E0070",
					"patch=0,EE,2030CD8C,extended,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Harry Potter and the Sorcerer's Stone (NTSC-U) [CRC: ] */
			else if (!strcmp(serial, "SLUS-20826")) 
			{
				int i;
				/* TODO/FIXME - decouple FPS unlock */
				char *patches[] = {
					"patch=0,EE,2026E528,extended,3405EA60",
					"patch=0,EE,0026E538,extended,24090001",
					"patch=0,EE,1026E914,extended,24030280",
					"patch=0,EE,202E0870,extended,24080001",
					"patch=0,EE,202E1078,extended,0000282D",
					"patch=0,EE,002E08B8,extended,24040002",
					"patch=0,EE,002E00C4,extended,30840002",
					"patch=0,EE,202E077C,extended,24A5FFFF",
					"patch=0,EE,202E1070,extended,24060050",
					"patch=0,EE,102E0854,extended,24030134"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* MotoGP 2 (NTSC-U) [CRC: 586EA828] */
			else if (!strcmp(serial, "SLUS-20285"))
			{
				/* Patch courtesy: asasega */
				int i;
				char *patches[] = {
					"patch=1,EE,20265444,extended,FD030000",
					"patch=1,EE,2027FED0,extended,24020001",
					"patch=1,EE,0043C588,extended,00000001",
					"patch=1,EE,0036C798,extended,00000003",
					"patch=1,EE,0036C7C0,extended,00000003"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));

				if (!strcmp(renderer, "paraLLEl-GS") || !strcmp(renderer, "Software"))
				{
					int i;
					char *patches[] = {
						/* full frame FMV only in software mode */
						"patch=1,EE,0036C798,extended,00000001",
						"patch=1,EE,0036C7C0,extended,00000001",
						"patch=1,EE,2036C7A0,extended,000018D8",
						"patch=1,EE,2036C7C8,extended,000018D8",
					};
					for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
						LoadPatchesFromString(std::string(patches[i]));
				}
			}
			/* MotoGP 3 (NTSC-U) [CRC: 46B7FEC5] */
			else if (!strcmp(serial, "SLUS-20625"))
			{
				/* Patch courtesy: asasega */
				int i;
				char *patches[] = {
					"patch=1,EE,202C16CC,extended,FD030000",
					"patch=1,EE,202DD564,extended,24020001",
					"patch=1,EE,003EF558,extended,00000003",
					"patch=1,EE,003EF580,extended,00000003"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Resident Evil - Code - Veronica X (NTSC-U) [CRC: 24036809] */
			else if (!strcmp(serial, "SLUS-20184"))
			{
				int i;
				char *patches[] = {
					"patch=0,EE,002CB0A4,extended,24060050",
					"patch=0,EE,202CB0A0,extended,0000282D",
					"patch=0,EE,202CB0B0,extended,00000000",
					"patch=0,EE,201002F4,extended,10A40029",
					"patch=0,EE,1010030C,extended,260202D0",
					"patch=0,EE,00100370,extended,26450023",
					"patch=0,EE,10100398,extended,64E30134",
					"patch=0,EE,102E1AF0,extended,24420134",
					"patch=0,EE,202EB944,extended,00000000",
					"patch=0,EE,202CB0F4,extended,0000482D",
					/* font fixes */
					"patch=1,EE,002B9A50,word,3C013F40",
					"patch=1,EE,002B9A54,word,44816000",
					"patch=1,EE,002B9A58,word,460C6B02",
					"patch=1,EE,002B9A5c,word,3C010050",
					"patch=1,EE,002B9A60,word,E42C8140",
					"patch=1,EE,002B9A64,word,E42D8138",
					"patch=1,EE,002B9A68,word,03E00008",
					"patch=1,EE,002B9A6c,word,E42E8130"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Resident Evil - Dead Aim (NTSC-U) [CRC: FBB5290C] */
			else if (!strcmp(serial, "SLUS-20669"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,2028A268,extended,00000050",
					"patch=1,EE,2028A274,extended,000001E0",
					"patch=1,EE,2028A284,extended,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Rumble Roses (NTSC-U) [CRC: C1C91715] */
			else if (!strcmp(serial, "SLUS-20970"))
			{
				/* Patch courtesy: felixthecat1970 */
				/* Framebuffer Display and no interlacing */
				if (!strcmp(renderer, "paraLLEl-GS"))
				{
					int i;
					char *patches[] = {
						"patch=1,EE,2010291C,extended,00000000",
						"patch=1,EE,20102B84,extended,00000000",
						"patch=1,EE,E0041100,extended,01D4ADA0",
						"patch=1,EE,21D4AD98,extended,00000001",
						"patch=1,EE,21D4ADA0,extended,00001000",
						"patch=1,EE,21D4ADC0,extended,00000001",
						"patch=1,EE,21D4ADC8,extended,00001000"
						/* TODO/FIXME - we're missing the upscaling 
						 * of the menu/startup screens */
					};
					for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
						LoadPatchesFromString(std::string(patches[i]));
				}
				else
				{
					int i;
					char *patches[] = {
						"patch=1,EE,2010291C,extended,00000000",
						"patch=1,EE,20102B84,extended,00000000",
						"patch=1,EE,E0041100,extended,01D4ADA0",
						"patch=1,EE,21D4AD98,extended,00000001",
						"patch=1,EE,21D4ADA0,extended,00001000",
						"patch=1,EE,21D4ADC0,extended,00000001",
						"patch=1,EE,21D4ADC8,extended,00001000"
					};
					for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
						LoadPatchesFromString(std::string(patches[i]));
				}
			}
			/* Shaun Palmer's Pro Snowboarder (NTSC-U) [CRC: 3A8E10D7] */
			else if (!strcmp(serial, "SLUS-20199"))
			{
				/* Patch courtesy: felixthechat1970 */
				int i;
				char *patches[] = {
					/* test s.backbuffer - frame mode by felixthecat1970 */
					/* menu is field render, use deinterlacing=auto */
					"patch=0,EE,2012B6C4,extended,0000102D",
					"patch=0,EE,2012B6E8,extended,00041803",
					"patch=0,EE,2012B714,extended,0000502D",
					"patch=0,EE,2012B730,extended,0000282D",
					"patch=0,EE,2012B750,extended,00083003",
					"patch=0,EE,2012B780,extended,0000502D"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Tales of Legendia (NTSC-U) [CRC: 43AB7214] */
			else if (!strcmp(serial, "SLUS-21201"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,D03F9750,extended,00001000",
					"patch=1,EE,103F9750,extended,000010E0"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Tekken Tag Tournament (NTSC-U) [CRC: 67454C1E] */
			else if (!strcmp(serial, "SLUS-20001")) 
			{
				int i;
				char *patches[] = {
					"patch=0,EE,20398960,extended,0000382D",
					"patch=0,EE,20398AF0,extended,0000502D",
					"patch=0,EE,10398AE0,extended,240701C0",
					"patch=0,EE,20398AF0,extended,0000502D",
					"patch=0,EE,10398B10,extended,240701C0",
					"patch=0,EE,10398B38,extended,240701C0",
					"patch=0,EE,20398B48,extended,0000502D"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Tekken 5 (NTSC-U) [CRC: 652050D2] */
			else if (!strcmp(serial, "SLUS-21059")) 
			{
				/* Patch courtesy: felixthecat1970 */
				/* TODO/FIXME - decouple widescreen */
				int i;
				char *patches[] = {
					"patch=0,EE,00D05EC8,extended,24050000",
					"patch=0,EE,00D05ECC,extended,24060050",
					"patch=0,EE,20D05ED4,extended,24070001",
					/* Devil Within upscaling */
					"patch=1,EE,E0078870,extended,01FFEF20",
					"patch=1,EE,202DE308,extended,AC940004", /* enable progressive at start - skips Starblade minigame */
					"patch=1,EE,202F06DC,extended,341B0001",
					"patch=1,EE,202F08FC,extended,A07B0000",
					/* sharp backbuffer main game - skips StarBlade intro game */
					"patch=1,EE,0031DA9C,extended,30630000",
					"patch=1,EE,00335A38,extended,24020001",
					"patch=1,EE,20335A5C,extended,00031C02",
					"patch=1,EE,20335E58,extended,00042402",
					/* Devil Within - sharp backbuffer */
					"patch=1,EE,E0020001,extended,0027E448",
					"patch=1,EE,2027E448,extended,00500000",
					"patch=1,EE,203F7330,extended,00500000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Urban Reign (NTSC-U) [CRC: BDD9BAAD] */
			else if (!strcmp(serial, "SLUS-21209"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,201372e0,extended,0C04DCEC",
					"patch=1,EE,201372e8,extended,0C04DCEC"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Whiplash (NTSC-U) [CRC: 4D22DB95] */
			else if (!strcmp(serial, "SLUS-20684"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,2025DFA4,extended,30630000",
					"patch=1,EE,20353958,extended,34030001",
					"patch=1,EE,2035396C,extended,34029040"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SCUS-", serial, strlen("SCUS-")))
		{
			/* God of War II (NTSC-U) [CRC: 2F123FD8] */
			if (!strcmp(serial, "SCUS-97481"))
			{
				int i;
				char *patches[] = {
					/* Default to progressive scan at first run */
					"patch=1,EE,0025a608,word,a04986dc",
					"patch=1,EE,001E45D4,word,24020001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Gran Turismo 4 (NTSC-U) [CRC: 77E61C8A] */
			else if (!strcmp(serial, "SCUS-97328"))
			{
				int i;
				char *patches[] = {
					/* Autoboot mode NTSC=0 / 480p=1 / 1080i=2 
					 * (change last number) or disable this code. */
					"patch=1,EE,20A461F0,extended,00000001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Ico (NTSC-U) [CRC: 6F8545DB] */
			else if (!strcmp(serial, "SCUS-97113")) 
			{
				int i;
				char *patches[] = {
					/* enable back buffer */
					"patch=0,EE,00274EF8,extended,00000001",
					"patch=0,EE,00274F20,extended,00000001",
					"patch=0,EE,00274F00,extended,00001040",
					"patch=0,EE,00274F28,extended,00001040",
					/* nointerlacing */
					"patch=1,EE,00274EF8,extended,00000001",
					"patch=1,EE,00274F20,extended,00000001",
					"patch=1,EE,00274F00,extended,00000040",
					"patch=1,EE,00274F28,extended,00000040"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Kinetica (NTSC-U) [CRC: D39C08F5] */
			else if (!strcmp(serial, "SCUS-97132"))
			{
				/* Patch courtesy: Mensa */
				/* Stops company logos and intro FMV from shaking. 
				 * Menus and in-game never had an issue */
				int i;
				char *patches[] = {
					"patch=1,EE,201ABB34,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SCES-", serial, strlen("SCES-")))
		{
			/* Ace Combat: Squadron Leader (PAL) [CRC: 1D54FEA9] */
			if (!strcmp(serial, "SCES-52424"))
			{
				int i;
				char *patches[] = {
					/* NOP the addition of front buffer address */
					"patch=1,EE,0032B0A8,word,00000000", /* 00A22825 */
					/* set the SMODE2 register to FRAME mode */
					"patch=1,EE,003311B8,word,00000000", /* 14400002 */
					/* force the 448 height for GS_DISPLAY2 
					 * register calculations (back buffer height is 448) */
					"patch=1,EE,00331124,word,241200E0", /* 00079403 */
					/* Last minute lazy fix for stuttering FMVs. Game does 
					 * render the prerecorded movies into the two interleaved 
					 * buffers. We need to remove the first patch when the 
					 * FMVs are played. */
					"patch=1,EE,E0011400,extended,0059660C",
					"patch=1,EE,2032B0A8,extended,00A22825"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Ico (PAL) [CRC: 5C991F4E] */
			else if (!strcmp(serial, "SCES-50760"))
			{
				/* Patch courtesy: agrippa */
				int i;
				char *patches[] = {
					/* Set the back buffer */
					"patch=1,EE,2028F500,extended,00001040",
					"patch=1,EE,2028F528,extended,00001040",
					/* Switch to the interlaced mode with FFMD set to 0. 
					 * Progressive mode, applied by default,
					 * does add a black bar at the bottom in the NTSC mode 
					 * when the back buffer is enabled */
					"patch=1,EE,2028F4F8,extended,00000001",
					"patch=1,EE,2028F520,extended,00000001",
					/* Check if the PAL mode is turned on to extend 
					 * the display buffer from 256 to 512 */
					"patch=1,EE,E0024290,extended,0028F508",
					"patch=1,EE,2028F50C,extended,001FF9FF",
					"patch=1,EE,2028F534,extended,001FF9FF",
					/* Check if the NTSC mode is turned on to extend 
					 * the display buffer from 224 to 448 */
					"patch=1,EE,E002927C,extended,0028F508",
					"patch=1,EE,2028F50C,extended,001DF9FF",
					"patch=1,EE,2028F534,extended,001DF9FF"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Tekken Tag Tournament (PAL) [CRC: 0DD8941C] */
			else if (!strcmp(serial, "SCES-50001")) 
			{
				int i;
				char *patches[] = {
					"patch=0,EE,203993D0,extended,0000382D",
					"patch=0,EE,10399580,extended,240700E0",
					"patch=0,EE,103995A8,extended,240701C0",
					"patch=0,EE,203995B8,extended,0000502D",
					"patch=0,EE,2039DDE8,extended,0000382D"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Tekken 4 (PAL) */
			else if (!strcmp(serial, "SCES-50878"))
			{
				/* Patch courtesy: felixthecat1970 */
				int i;
				char *patches[] = {
					"patch=0,EE,001E2254,extended,24020002",
					"patch=0,EE,0022B138,extended,24050006",
					"patch=0,EE,001EDC24,extended,24020009"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SLES-", serial, strlen("SLES-")))
		{
			/* Colin McRae Rally 3 (PAL) [CRC: 7DEAE69C] */
			if (!strcmp(serial, "SLES-51117")) 
			{
				/* Patch courtesy: agrippa */
				int i;
				char *patches[] = {
					"patch=1,EE,00246B90,word,24040001", 
					/* set FFMD to 0 in SMODE2 register to 
					 * disable field mode */
					"patch=1,EE,00247A64,word,00000000"  
					/* NOP the switch to the front buffer 
					 * A full height back buffer enabled, 
					 * instead of a downsampled front buffer. */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Resident Evil - Dead Aim (PAL) [CRC: F79AF536] */
			else if (!strcmp(serial, "SLES-51448")) 
			{
				/* Patch courtesy: dante3732 */
				int i;
				char *patches[] = {
					"patch=1,EE,2028AB88,extended,00000050",
					"patch=1,EE,2028AB94,extended,000001E0",
					"patch=1,EE,2028ABA4,extended,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Star Ocean: Til the End of Time (PAL) [CRC: E04EA200] */
			else if (!strcmp(serial, "SLES-82028"))
			{
				/* Patch courtesy: agrippa */
				int i;
				char *patches[] = {
					/* Skip the integrity check of the main executable file */
					"patch=1,EE,E0110011,extended,001F7660",
					"patch=1,EE,201e2530,extended,10000016",
					"patch=1,EE,201e2ff8,extended,10000016",
					"patch=1,EE,201e3410,extended,10000016",
					"patch=1,EE,201e3758,extended,10000016",
					"patch=1,EE,201e3968,extended,10000016",
					"patch=1,EE,201e3ba8,extended,10000016",
					"patch=1,EE,201e3d00,extended,10000016",
					"patch=1,EE,201eb5f8,extended,10000016",
					"patch=1,EE,201f68c0,extended,10000016",
					"patch=1,EE,201f6bb0,extended,10000016",
					"patch=1,EE,201f6c50,extended,10000016",
					"patch=1,EE,201f7030,extended,10000016",
					"patch=1,EE,201f7160,extended,10000016",
					"patch=1,EE,201f72a0,extended,10000016",
					"patch=1,EE,201f73d0,extended,10000016",
					"patch=1,EE,201f7500,extended,10000016",
					"patch=1,EE,201f7660,extended,10000016",
					/* in-battle anti-cheat checks? I have 
					 * not seen the game to get there though. */
					"patch=1,EE,E002FFFA,extended,001EDB44",
					"patch=1,EE,201EDB44,extended,1400fffa",
					"patch=1,EE,201E94E0,extended,1000000F", /* 1440000F */
					/* full height frame buffer and video mode patches */
					"patch=0,EE,00101320,word,A0285C84", /* A0205C84 */
					"patch=1,EE,0012EF60,word,00000000", /* 10C00005 */
					"patch=1,EE,00100634,word,24050001", /* 0000282D */
					"patch=1,EE,00100638,word,24060003", /* 24060050 */
					"patch=1,EE,00100640,word,24070000", /* 24070001 */
					/* Texture fix for the battle mode */
					"patch=1,EE,E0011183,extended,001E0784",
					"patch=1,EE,201E0784,extended,24021D00"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SLPS-", serial, strlen("SLPS-")))
		{
			/* Alpine Racer 3 (NTSC-J) [CRC: 771C3B47] */
			if (!strcmp(serial, "SLPS-20181"))
			{
				/* Patch courtesy: asasega */
				int i;
				char *patches[] = {
					"patch=1,EE,E00410E0,extended,00686C80",
					"patch=1,EE,20686C78,extended,00000001",
					"patch=1,EE,20686C80,extended,00001000",
					"patch=1,EE,20686CA0,extended,00000001",
					"patch=1,EE,20686CA8,extended,00001000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SLPM-", serial, strlen("SLPM-")))
		{
			/* Mushihimesama (NTSC-J) [CRC: F0C24BB1] */
			if (!strcmp(serial, "SLPM-66056"))
			{
				/* Patch courtesy: asasega */
				int i;
				char *patches[] = {
					"patch=1,EE,2010C300,extended,34030001",
					"patch=1,EE,2010C314,extended,3402148C",
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Rumble Fish, The (NTSC-J) */
			else if (!strcmp(serial, "SLPM-65919"))
			{
				/* Patch courtesy: felixthecat1970 */
				int i;
				char *patches[] = {
					/* Framebuffer + 480p mode + No interlacing */
					"patch=0,EE,201102A4,extended,3C050000",
					"patch=0,EE,201102AC,extended,3C060050",
					"patch=0,EE,201102B4,extended,3C070001",
					"patch=0,EE,20110948,extended,34030002",
					"patch=1,EE,2034FD50,extended,00009446",
					"patch=1,EE,2034FD5C,extended,001DF4FF",
					"patch=1,EE,2034FD78,extended,00009446",
					"patch=1,EE,2034FD84,extended,001DF4FF",
					/* NULL Int ints */
					"patch=0,EE,20111278,extended,03E00008",
					"patch=0,EE,2011127C,extended,00000000",
					"patch=0,EE,201114E0,extended,03E00008",
					"patch=0,EE,201114E4,extended,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Sega Rally 2006 (NTSC-J) [CRC: B26172F0] */
			else if (!strcmp(serial, "SLPM-66212"))
			{
				/* Patch courtesy: asasega */
				int i;
				char *patches[] = {
					"patch=1,EE,20106FA0,extended,34030001",
					"patch=1,EE,20106FB4,extended,34021040"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}

	}

	if (disable_mipmaps == 1)
	{
		/* The games listed below need patches when mipmapping
		 * is set to unclamped */

		if (!strncmp("SLUS-", serial, strlen("SLUS-")))
		{
			/* Ace Combat 5 - The Unsung War (NTSC-U) [CRC: 39B574F0] */
			if (!strcmp(serial, "SLUS-20851"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,0011F2CC,word,00000000",
					"patch=1,EE,0011F2DC,word,00000000",
					"patch=1,EE,0011F2E8,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Aggressive Inline (NTSC-U) [CRC: ] */
			else if (!strcmp(serial, "SLUS-20327"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,001090B0,word,45010009"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Ape Escape 2 (NTSC-U) [CRC: BDD9F5E1] */
			else if (!strcmp(serial, "SLUS-20685"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,0034CE88,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* BMX XXX (SLUS-20415) [CRC: 2999BCF9] */
			else if (!strcmp(serial, "SLUS-20415"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,00108610,word,10000009"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* F1 Career Challenge (SLUS-20693) [CRC: 2C1173B0] */
			else if (!strcmp(serial, "SLUS-20693"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,00257a40,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Harry Potter - Quidditch World Cup (NTSC-U) [CRC: 39E7ECF4] */
			else if (!strcmp(serial, "SLUS-20769"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,002ABD7C,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Lara Croft Tomb Raider - Legend (NTSC-U) [CRC: BC8B3F50] */
			else if (!strcmp(serial, "SLUS-21203"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,00127390,word,10000022"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Legacy of Kain: Soul Reaver, The (NTSC-U) [CRC: 1771BFE4]) */
			else if (!strcmp(serial, "SLUS-20165"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,0029FC00,word,000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Quake III - Revolution (NTSC-U) [CRC: A56A0525] */
			else if (!strcmp(serial, "SLUS-20167"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,002D0398,word,03E00008"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Whiplash (NTSC-U) [CRC: 4D22DB95] */
			else if (!strcmp(serial, "SLUS-20684"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,0025D19C,word,10000007"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Harry Potter and the Goblet of Fire (NTSC-U) [CRC: B38CC628] */
			else if (!strcmp(serial, "SLUS-21325"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,002CF158,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SCUS-", serial, strlen("SCUS-")))
		{
			/* Jak II: Renegade (NTSC-U) [CRC: 9184AAF1] */
			if (!strcmp(serial, "SCUS-97265"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,005F8D08,word,10000016"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Jak III (NTSC-U) [CRC: 644CFD03] */
			else if (!strcmp(serial, "SCUS-97330"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,0059F570,word,10000016"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Jak X [CRC: 3091E6FB] */
			else if (!strcmp(serial, "SCUS-97574"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,007AEB70,word,10000016",
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SLES-", serial, strlen("SLES-")))
		{
			/* Aggressive Inline (PAL) [CRC: ] */
			if (!strcmp(serial, "SLES-50480"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,00109130,word,45010009"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* BMX XXX (SLES-51365) [CRC: ] */
			else if (!strcmp(serial, "SLES-51365"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,00108780,word,10000009"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* F1 Career Challenge (SLES-51584) [CRC: 2C1173B0] */
			else if (!strcmp(serial, "SLES-51584"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,00257a40,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Harry Potter - Quidditch World Cup (PAL) */
			else if (!strcmp(serial, "SLES-51787"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,002ABD4C,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Harry Potter and the Goblet of Fire (NTSC-U) [CRC: B38CC628] */
			else if (    
					   !strcmp(serial, "SLES-53728")
					|| !strcmp(serial, "SLES-53727")
					|| !strcmp(serial, "SLES-53726")
				)
			{
				int i;
				char *patches[] = {
					"patch=1,EE,002CF158,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SLPM-", serial, strlen("SLPM-")))
		{
			/* Harry Potter - Quidditch World Cup (NTSC-J) */
			if (!strcmp(serial, "SLPM-62408"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,002ABC04,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SLPS-", serial, strlen("SLPS-")))
		{
			/* F1 Career Challenge (NTSC-J) [CRC: 5CBB11E6] */
			if (!strcmp(serial, "SLPS-20295"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,002581d8,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
	}

	var.key = "pcsx2_fastcdvd";
	if (       environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) 
		&& var.value && !strcmp(var.value, "enabled"))
	{
		/* Shadow Man: 2econd Coming (NTSC-U) [CRC: 60AD8FA7] */
		if (!strcmp(serial, "SLUS-20413"))
		{
			/* Only works with fastcdvd when enabling these patches */
			int i;
			char *patches[] = {
				"patch=1,IOP,000884e8,word,34048800",
				"patch=1,IOP,000884ec,word,34048800",
				"patch=1,IOP,00088500,word,34048800",
				"patch=1,IOP,0008850c,word,34048800",
				"patch=1,IOP,000555e8,word,34048800",
				"patch=1,IOP,000555ec,word,34048800",
				"patch=1,IOP,00055600,word,34048800",
				"patch=1,IOP,0005560c,word,34048800"
			};
			for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
				LoadPatchesFromString(std::string(patches[i]));
		}
	}

	if (game_enhancements)
	{
		if (!strncmp("SCUS-", serial, strlen("SCUS-")))
		{
			/* Dark Cloud (NTSC-U) [CRC: A5C05C78] */
			if (!strcmp(serial, "SCUS-97111"))
			{
				int i;
				char *patches[] = {
					/* CNP Draw Distance */
					"patch=1,EE,00156554,word,00000000",
					"patch=1,EE,00156678,word,3C1943C0",
					"patch=1,EE,00156684,word,AC990CEC",
					"patch=1,EE,001729DC,word,00000000",
					"patch=1,EE,00155FF0,word,00000000",
					/* LOD Distance */
					"patch=1,EE,00157364,word,10000006",
					/* Shade */
					"patch=1,EE,001A3E80,word,C6800004",
					/* Additional Shade */
					"patch=1,EE,001A3D80,word,C6800004"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Downhill Domination (NTSC-U) [CRC: 5AE01D98] */
			else if (!strcmp(serial, "SCUS-97177"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,0029DAA8,word,00000000" /* Max LOD Distance */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* God of War II (NTSC-U) [CRC: 2F123FD8] */
			else if (!strcmp(serial, "SCUS-97481"))
			{
				int i;
				char *patches[] = {
					/* Allow MPEG skip by pressing x */
					"patch=1,EE,001DD8C8,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Gran Turismo 4 (NTSC-U) [CRC: 77E61C8A] */
			else if (!strcmp(serial, "SCUS-97328"))
			{
				int i;
				char *patches[] = {
					/* Max LOD cars */
					"patch=1,EE,204539C0,extended,10000009",
					"patch=1,EE,20454FBC,extended,1000000E"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SLUS-", serial, strlen("SLUS-")))
		{
			/* Burnout 3: Takedown (NTSC-U) [CRC: D224D348] */
			if (!strcmp(serial, "SLUS-21050"))
			{
				int i;
				char *patches[] = {
					/* Enable props in Road Rage mode */
					"patch=0,EE,201B9F60,extended,00000000",
					"patch=0,EE,202F9A44,extended,00000000",
					/* Unlimited explosions (also affects crash mode) */
					"patch=0,EE,201BBA08,extended,00000000",
					/* Render extra particles while driving */
					"patch=0,EE,20261EB8,extended,24040001",
					/* Use 255 colors in garage. 
					 * (Doesn't jump to 254 after the 8th color.) */
					"patch=1,EE,2042BCE8,extended,70A028E8",
					/* bypass PVS/force render all immediate units */
					"patch=1,EE,20301EAC,extended,00000000",
					/* Force specific LOD */
					"patch=0,EE,00151ABF,extended,00000010",
					/* Last digit is LOD level, 
					 * 0, 1, 2, 3, and 4 (4 being the most detailed iirc) */
					"patch=0,EE,20151B78,extended,24070004",
					"patch=0,EE,20261E6C,extended,24120001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Burnout Revenge (NTSC-U) [CRC: D224D348] */
			else if (!strcmp(serial, "SLUS-21242"))
			{
				int i;
				char *patches[] = {
					/* Enable props in World Tour Road Rage */
					"patch=0,EE,20129FF8,extended,00000000",
					/* Enable props in Multiplayer/Single Event Road Rage */
					"patch=0,EE,2012648C,extended,00000000",
					/* Enable props in Traffic Attack mode */
					"patch=0,EE,20123C1C,extended,00000000",
					/* Force race cars LOD to 5 */
					"patch=0,EE,202D1660,extended,03E00008",
					"patch=0,EE,202D1664,extended,24020004",
					/* Prevent race cars reflections from fading further away */
					"patch=0,EE,202D165C,extended,E4C30000",
					/* Falling car parts while driving 
					 * (takedowns and traffic checks) */
					"patch=0,EE,20210FA8,extended,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Dynasty Warriors 4 (NTSC-U) [CRC: 6C89132B] [UNDUB] [CRC: 6C881C2B] */
			else if (!strcmp(serial, "SLUS-20653"))
			{
				int i;
				char *patches[] = {
					/* Disable Distance Based Model Disappearing */
					"patch=1,EE,001ce0d0,word,00000000",
					/* High LOD */
					"patch=1,EE,0018C8d0,word,00000000",
					"patch=1,EE,0018CE9C,word,00000000",
					/* Skip Events with X Button (DUELS ACCEPT IS SQUARE) */
					"patch=1,EE,0020BB98,word,24034008",
					"patch=1,EE,0020BA94,word,30638000",
					/* FMV Skip with X button */
					"patch=1,EE,002100A4,word,30424008",
					/* Able to Skip Koei Logo */
					"patch=1,EE,00362CEC,word,00210090",
					/* Increase default of 24 max units 
					 * rendered at the same time to 26. */
					"patch=1,EE,001CDFB0,word,2403001a"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Dynasty Warriors 4: Empires (NTSC-U] [CRC: BD3DBCF9] */
			else if (!strcmp(serial, "SLUS-20938"))
			{
				int i;
				char *patches[] = {
					/* Increased draw distance Empires */
					"patch=1,EE,0015648C,word,00000000",
					"patch=1,EE,0015643C,word,00000000",
					"patch=1,EE,20508F1C,word,463b8000", /* 1P Mode */
					"patch=1,EE,20508F40,word,463b8000",
					"patch=1,EE,20508F64,word,463b8000",
					"patch=1,EE,20508FAC,word,463b8000",
					"patch=1,EE,20508FD0,word,463b8000",
					"patch=1,EE,20508F18,word,4633b000",
					"patch=1,EE,20508F3c,word,4633b000",
					"patch=1,EE,20508F60,word,4633b000",
					"patch=1,EE,20508FA8,word,4633b000",
					"patch=1,EE,20508Fcc,word,4633b000",
					/* Increases default of 24 maximum units 
					 * rendered at the same time to 28. */
					"patch=1,EE,001cbd34,word,2402001c"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* SSX3 (NTSC-U) [CRC: 08FFF00D] */
			else if (!strcmp(serial, "SLUS-20772"))
			{
				int i;
				char *patches[] = {
					/* Disable intro videos (ea / thx / splash) */
					"patch=0,EE,001A2840,word,0000202D",
					"patch=0,EE,001A2864,word,0000202D",
					"patch=0,EE,001A28DC,word,0000202D",
					/* Disable perf frame skip (metro slowdown) */
					"patch=0,EE,00230704,word,00000000",
					"patch=0,EE,00230710,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SCPS-", serial, strlen("SCPS-")))
		{
			/* Gran Turismo 3 A-Spec (NTSC-J) [CRC: 9DE5CF65] */
			if (!strcmp(serial, "SCPS-15009"))
			{
				int i;
				char *patches[] = {
					/* Max car LODs */
					"patch=1,EE,21BD8A,short,1000",
					"patch=1,EE,21CA16,short,1000",
					"patch=1,EE,21F2E2,short,1000",
					"patch=1,EE,2212A2,short,1000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Gran Turismo 4 Prologue (NTSC-J) [CRC: EF258742] */
			else if (!strcmp(serial, "SCPS-15055"))
			{
				int i;
				char *patches[] = {
					/* car higher LOD - higher LOD wheels */
					"patch=1,EE,2057702C,extended,756E656D",
					"patch=1,EE,00577030,extended,0000002F",
					"patch=1,EE,2055C344,extended,6E656D2F",
					"patch=1,EE,2055C348,extended,73252F75"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SLPM-", serial, strlen("SLPM-")))
		{
			/* Sega Rally 2006 (NTSC-J) [CRC: B26172F0] */
			if (!strcmp(serial, "SLPM-66212"))
			{
				int i;
				char *patches[] = {
					/* Render Distance Patch (required, adds +25%) */
					"patch=1,EE,2017B150,extended,00000000", 
					/* +100% Render Distance (0.35f, max without glitching) */
					"patch=1,EE,203832EC,word,3EB33333"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
	}

	if (uncapped_framerate)
	{
		if (!strncmp("SLUS-", serial, strlen("SLUS-")))
		{
			/* 24 - The Game (NTSC-U) [CRC: F1C7201E] */
			if (!strcmp(serial, "SLUS-21268"))
			{
				/* Patch courtesy: Red-tv */
				/* 60fps uncapped. Need EE Overclock at 180%. */
				int i;
				char *patches[] = {
					/* 60fps */
					"patch=1,EE,005F9808,word,00000001",
					/* Fix FMV */
					"patch=1,EE,e0010001,extended,0058EEF4",
					"patch=1,EE,205F9808,extended,00000002"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Aeon Flux (NTSC-U) [CRC: 9FA0A1B0] */
			else if (!strcmp(serial, "SLUS-21205"))
			{
				/* 60fps uncapped. Need EE Overclock at 300%. */
				int i;
				char *patches[] = {
					"patch=1,EE,2036C438,extended,28630001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Alias (NTSC-U) [CRC: E3ADDC73] */
			else if (!strcmp(serial, "SLUS-20673"))
			{
				/* 60fps uncapped. Need EE Overclock at 130%. */
				int i;
				char *patches[] = {
					"patch=1,EE,2024BEF0,extended,14400039" /* 10400039 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Baroque (NTSC-U) [CRC: 4566213C] */
			else if (!strcmp(serial, "SLUS-21714"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,00556E70,word,00000000",
					/* Revert to 30fps in FMV and cutscenes */
					"patch=1,EE,e0010000,extended,005179C0",
					"patch=1,EE,20556E70,extended,00000001",
					/* Player Speed Modifier */
					"patch=1,EE,0013D770,word,3C033F00",
					"patch=1,EE,00143CA4,word,3C023F00",
					"patch=1,EE,00146FEC,word,3C033F00",
					/* Enemy and NPC Animation Speed Modifier */
					"patch=1,EE,00146E08,word,3C023f00",
					"patch=1,EE,00146DF0,word,3C033eCC",
					/* Camera Speed Modifier */
					"patch=1,EE,0013DCBC,word,3C023F80",
					/* Player's Gauge Speed Modifier */
					"patch=1,EE,001341d8,word,3c024000",
					"patch=1,EE,00133ff4,word,3c024000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Batman - Rise of Sin Tzu (NTSC-U) [CRC: 24280F22] */
			else if (!strcmp(serial, "SLUS-20709"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped. */
				int i;
				char *patches[] = {
					"patch=1,EE,00534720,word,00000001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Black (NTSC-U) [CRC: 5C891FF1] */
			else if (!strcmp(serial, "SLUS-21376"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,1040DF74,extended,00000001", /* 60 fps */
					"patch=1,EE,205A8A9C,extended,3C888889", /* speed */
					"patch=1,EE,204BC13C,extended,3C888889",
					"patch=1,EE,2040EBAC,extended,3C888889"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Burnout 3: Takedown (NTSC-U) [CRC: D224D348] */
			else if (!strcmp(serial, "SLUS-21050"))
			{
				int i;
				char *patches[] = {
					/* Fix FMVs playback speed while using 60 FPS patches */
					"patch=0,EE,201D3F2C,extended,1000000A",
					"patch=0,EE,20130DD8,extended,C7958074",
					"patch=0,EE,20130DDC,extended,3C084000",
					"patch=0,EE,20130DE0,extended,4488A000",
					"patch=0,EE,20130DE4,extended,4614AD03",
					"patch=0,EE,20130DE8,extended,00000000",

					"patch=0,EE,201320D8,extended,1000004B",

					"patch=0,EE,20437758,extended,100000F1"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Burnout Revenge (NTSC-U) [CRC: D224D348] */
			else if (!strcmp(serial, "SLUS-21242"))
			{
				int i;
				char *patches[] = {
					/* 60fps Split Screen */
					"patch=1,EE,20104BC0,extended,080680A0",
					"patch=1,EE,20104BC4,extended,00000000",
					/* 60 FPS Front End */
					"patch=1,EE,201125F4,word,24040001",
					"patch=1,EE,201125EC,word,00108002",
					/* 60 FPS Crashes & Crash Mode */
					"patch=1,EE,20104B9C,word,90850608"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Cold Fear (NTSC-U) [CRC: ECFBAB36] */
			else if (!strcmp(serial, "SLUS-21047"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,0046E484,extended,00000001" /* 60fps */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Dark Angel - James Cameron's (NTSC-U) [CRC: 29BA2F04] */
			else if (!strcmp(serial, "SLUS-20379"))
			{
				/* Patch courtesy: PeterDelta */
				/* 60fps uncapped. Need EE Overclock at 130% */
				int i;
				char *patches[] = {
					"patch=1,EE,0027F154,word,10400012"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Dawn of Mana (NTSC-U) [CRC: 9DC6EE5A] */
			else if (!strcmp(serial, "SLUS-21574"))
			{
				/* 60fps uncapped. */
				int i;
				char *patches[] = {
					/* 28620002 fps without doubling speed */
					"patch=1,EE,20113010,extended,28620001",
					/* condition to avoid hang and skip FMVs */
					"patch=1,EE,E0010001,extended,005D7338", 
					"patch=1,EE,20113010,extended,28620002"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Destroy All Humans! (NTSC-U) [CRC: 67A29886] */
			else if (!strcmp(serial, "SLUS-20945"))
			{
				/* 60fps uncapped. */
				int i;
				char *patches[] = {
					"patch=1,EE,203EF80C,extended,00000001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Deus Ex: The Conspiracy (NTSC) [CRC: 3AD6CF7E] */
			else if (!strcmp(serial, "SLUS-20111"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped. Need EE Overclock to be stable. */
				int i;
				char *patches[] = {
					"patch=1,EE,2030D234,word,28420001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Echo Night - Beyond (NTSC) [CRC: 2DE16D21] */
			else if (!strcmp(serial, "SLUS-20928"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped. Need EE Overclock at 130%. */
				int i;
				char *patches[] = {
					"patch=1,EE,2013FFDC,word,10000014"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Grand Theft Auto III (NTSC-U) [CRC: 5E115FB6] */
			else if (!strcmp(serial, "SLUS-20062"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,2027CEAC,extended,28420001" /* 60fps */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Grand Theft Auto: Vice City (NTSC-U) [CRC: 20B19E49] */
			else if (!strcmp(serial, "SLUS-20552"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,20272204,extended,28420001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Grand Theft Auto: San Andreas (NTSC-U) [CRC: 399A49CA] */
			else if (!strcmp(serial, "SLUS-20946"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=0,EE,2039B53C,extended,24040001", /* Set VSync Mode to 60 FPS */
					"patch=1,EE,0066804C,word,10000001",
					"patch=1,EE,D066804C,word,10000001",
					"patch=1,EE,006678CC,extended,00000001" /* Framerate boost */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Jurassic: The Hunted (NTSC-U) [CRC:EFE4448F] */
			else if (!strcmp(serial, "SLUS-21907"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,2017D480,word,2C420001" /* 60fps */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Lord of the Rings, Return of the King (NTSC-U) [CRC: 4CE187F6] */
			else if (!strcmp(serial, "SLUS-20770"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,2014B768,extended,10000013" /* 14400003 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Max Payne 2: The Fall of Max Payne (NTSC-U) [CRC: CD68E44A] */
			else if (!strcmp(serial, "SLUS-20814"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,005D8DF8,word,00000001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Metal Gear Solid 2: Substance (NTSC-U) [CRC: ] */
			else if (!strcmp(serial, "SLUS-20554"))
			{
				/* Patch courtesy: flcl8193 */
				/* 60fps uncapped cutscenes. */
				int i;
				char *patches[] = {
					"patch=1,EE,001914F4,word,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Metal Gear Solid 3: Subsistence (NTSC-U) (Disc 1) [CRC: ] */
			else if (!strcmp(serial, "SLUS-21359"))
			{
				/* Patch courtesy: felixthecat1970 */
				int i;
				char *patches[] = {
					/* override FPS lock functions calls, 
					 * disable others FPS codes */
					"patch=1,EE,20145830,extended,0C03FFE8",
					"patch=1,EE,200FFFA0,extended,241B0001",
					"patch=1,EE,200FFFA4,extended,145B0008",
					"patch=1,EE,200FFFA8,extended,00000000",
					"patch=1,EE,200FFFAC,extended,149B0006",
					"patch=1,EE,200FFFB0,extended,00000000",
					"patch=1,EE,200FFFB4,extended,161B0004",
					"patch=1,EE,200FFFB8,extended,00000000",
					"patch=1,EE,200FFFBC,extended,0000102D",
					"patch=1,EE,200FFFC0,extended,0000202D",
					"patch=1,EE,200FFFC4,extended,0000802D",
					"patch=1,EE,200FFFC8,extended,03E00008",
					"patch=1,EE,20145570,extended,24060001",
					"patch=1,EE,201453B4,extended,240B0001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Metal Arms - Glitch in the System (NTSC-U) [CRC: E8C504C8] */
			else if (!strcmp(serial, "SLUS-20786"))
			{
				/* Patch courtesy: PeterDelta */
				/* 60fps uncapped. Need EE Overclock at 180%. */
				int i;
				char *patches[] = {
					"patch=1,EE,004B2C98,word,00000001" /* 00000002 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Midnight Club 3 - DUB Edition (NTSC-U) v1.0 [CRC: 4A0E5B3A] */
			else if (!strcmp(serial, "SLUS-21029"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,00617AB4,word,00000001" /* 00000002 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Need For Speed - Hot Pursuit 2 (NTSC-U) [CRC: 1D2818AF] */
			else if (!strcmp(serial, "SLUS-20362"))
			{
				/* Patch courtesy: felixthecat1970 */
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=0,EE,0032F638,extended,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Need For Speed Underground 1 (NTSC-U) [CRC: CB99CD12] */
			else if (!strcmp(serial, "SLUS-20811"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,2011060C,word,2C420001" /* 60fps */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Need For Speed Underground 2 (NTSC-U) [CRC: F5C7B45F] */
			else if (!strcmp(serial, "SLUS-21065"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,201D7ED4,word,2C420001" /* 60fps */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Power Rangers - Dino Thunder (NTSC-U) [CRC: FCD89DC3] */
			else if (!strcmp(serial, "SLUS-20944"))
			{
				/* Patch courtesy: felixthecat1970 */
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=0,EE,101400D4,extended,2403003C",
					"patch=0,EE,2020A6BC,extended,241B0001",
					"patch=0,EE,2020A6C4,extended,03E00008",
					"patch=0,EE,2020A6C8,extended,A39B8520",
					"patch=0,EE,2020A7A8,extended,241B0002",
					"patch=0,EE,2020A7F8,extended,A39B8520"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Prince of Persia: The Sands of Time (NTSC-U) [CRC: 7F6EB3D0] */
			else if (!strcmp(serial, "SLUS-20743"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped. Need EE Overclock at 180%. */
				int i;
				char *patches[] = {
					"patch=1,EE,0066D044,word,00000001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Psi-Ops: The Mindgate Conspiracy (NTSC-U) [CRC: 9C71B59E] */
			else if (!strcmp(serial, "SLUS-20688"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,2017AB28,extended,00000000" /* 1640FFE5 fps1 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Red Faction (NTSC-U) [CRC: FBF28175] */
			else if (!strcmp(serial, "SLUS-20073"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,20164F9C,extended,24040001" /* 60fps */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Red Faction II (NTSC-U) [CRC: 8E7FF6F8] */
			else if (!strcmp(serial, "SLUS-20442"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,201218A0,word,24040001" /* 60fps */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Reign of Fire (NTSC-U) [CRC: D10945CE] */
			else if (!strcmp(serial, "SLUS-20556"))
			{
				/* Patch courtesy: Gabominated */
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,00264E70,word,00000001", /* 00000002 */
					"patch=1,EE,001409b4,word,2402003c"  /* 2402001e native global speed */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));

			}
			/* Return to Castle Wolfenstein: Operation Resurrection (NTSC-U) [CRC: 5F4DB1DD] */
			else if (!strcmp(serial, "SLUS-20297"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=0,EE,2017437C,word,2C420001" /* 60fps */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Rune - Viking Warlord (NTSC-U) [CRC: 1259612B] */
			else if (!strcmp(serial, "SLUS-20109"))
			{
				/* Patch courtesy: PeterDelta */
				/* 60fps uncapped. Need EE Overclock at 180%. */
				int i;
				char *patches[] = {
					"patch=1,EE,001305A4,extended,28420001" /* 28420002 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Scarface - The World is Yours (NTSC-U) [CRC: 41F4A178] */
			else if (!strcmp(serial, "SLUS-21111"))
			{
				/* 60fps uncapped. Need EE Overclock at 180%. */
				int i;
				char *patches[] = {
					"patch=1,EE,20DAFABC,word,00000000" /* 00000001 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Serious Sam - Next Encounter (NTSC-U) [CRC: 155466E8] */
			else if (!strcmp(serial, "SLUS-20907"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=0,EE,20127580,extended,00000000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Sonic Heroes (NTSC-U) [CRC: 78FF4E3B] */
			else if (!strcmp(serial, "SLUS-20718"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped. */
				int i;
				char *patches[] = {
					"patch=1,EE,004777C0,word,00000001", /* fps */
					"patch=1,EE,2028FF5C,word,24020001" /* speed */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Spawn - Armageddon (NTSC-U) [CRC: B7E7D66F] */
			else if (!strcmp(serial, "SLUS-20707"))
			{
				/* Patch courtesy: PeterDelta */
				/* 60fps uncapped. Need EE Overclock at 130%. */
				int i;
				char *patches[] = {
					"patch=0,EE,00226830,word,24020001" /* 24020002 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Splinter Cell - Pandora Tomorrow (NTSC-U) [CRC: 0277247B] */
			else if (!strcmp(serial, "SLUS-20958"))
			{
				/* Patch courtesy: PeterDelta */
				/* 60fps uncapped. Need EE Overclock at 130%. */
				int i;
				char *patches[] = {
					"patch=1,EE,0018D778,word,24030001" /* 24030002 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* SSX On Tour (NTSC-U) [CRC: 0F27ED9B] */
			else if (!strcmp(serial, "SLUS-21278"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					/* Forces the FrameHalver variable to 1
					 * 1 = 60fps, 2 = 30fps, and probably 3 = 15fps. */
					"patch=1,EE,003132b4,extended,01001124",
					/* Skipping some nonsense code that's probably 
					 * no longer needed */
					"patch=1,EE,003132b8,extended,15000010"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Summoner 2 (NTSC-U) [CRC: 93551583] */
			else if (!strcmp(serial, "SLUS-20448"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=0,EE,2017BC34,word,24040001" /* 60fps */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Super Monkey Ball Deluxe (NTSC-U) [CRC: 43B1CD7F] */
			else if (!strcmp(serial, "SLUS-20918"))
			{
				/* Patch courtesy: gamehacking.org, by Josh_7774, & Gabominated, PCSX2 forum */
				/* 60fps uncapped. Breaks Golf & Tennis. */
				int i;
				char *patches[] = {
					"patch=1,EE,20146D04,extended,24020001",
					/* Following patches fixes FMVs */
					"patch=1,EE,004C318C,extended,00000001", 
					"patch=1,EE,E0010001,extended,00473478",
					"patch=1,EE,204C318C,extended,00000002"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Unreal Tournament (NTSC-U) [CRC: 5751CAC1] */
			else if (!strcmp(serial, "SLUS-20034"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,0012D134,extended,28420001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* XGRA - Extreme G Racing Association (NTSC-U) [CRC: 56B36513] */
			else if (!strcmp(serial, "SLUS-20632"))
			{
				/* Patch courtesy: PeterDelta */
				/* 60fps uncapped. Need EE Overclock at 130%. */
				int i;
				char *patches[] = {
					"patch=1,EE,002052B4,extended,30420004",
					"patch=1,EE,E0010000,extended,01FFE32C",
					"patch=1,EE,002052B4,extended,30420008"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SLES-", serial, strlen("SLES-")))
		{
			/* Aeon Flux (PAL-M) [CRC: 761CABB3] */
			if (!strcmp(serial, "SLES-54169"))
			{
				/* 50fps uncapped. Need EE Overclock at 300%. */
				int i;
				char *patches[] = {
					"patch=1,EE,00362EB8,word,28630001" /* 28630002 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Alias (PAL-M) [CRC: 83466553] */
			if (!strcmp(serial, "SLES-51821"))
			{
				/* 50fps uncapped. Need EE Overclock at 130%. */
				int i;
				char *patches[] = {
					"patch=1,EE,0024BEAC,word,2C420000", /* 2C42001E */
					"patch=1,EE,001DED08,word,3C013F00"  /* 3C013F80 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Dark Angel (PAL) [CRC: 5BE3F481] */
			else if (!strcmp(serial, "SLES-53414"))
			{
				/* Patch courtesy: PeterDelta */
				/* Uncapped. Need EE Overclock at 130% */
				int i;
				char *patches[] = {
					"patch=1,EE,00280B74,word,1040000D"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Echo Night - Beyond (PAL) [CRC: BBF8C3D6] */
			else if (!strcmp(serial, "SLES-53414"))
			{
				/* Patch courtesy: PeterDelta */
				/* 60fps uncapped. Need EE Overclock at 130%. Select 60Hz */
				int i;
				char *patches[] = {
					"patch=1,EE,E001001E,extended,0028A348",
					"patch=1,EE,0028A348,extended,0000003C"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Metal Arms - Glitch in the System (PAL) [CRC: AF399CCC] */
			else if (!strcmp(serial, "SLES-51758"))
			{
				/* Patch courtesy: PeterDelta */
				/* 50fps uncapped. Need EE Overclock at 180%. */
				int i;
				char *patches[] = {
					"patch=1,EE,004BEA90,word,00000001" /* 00000002 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Psi-Ops: The Mindgate Conspiracy (PAL-M) [CRC: 5E7EB5E2] */
			else if (!strcmp(serial, "SLES-52702"))
			{
				/* Patch courtesy: PeterDelta */
				/* 50/60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,0017ACD8,word,00000000" /* 1640FFE5 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Reign of Fire (PAL) [CRC: 79464D5E] */
			else if (!strcmp(serial, "SLES-50873"))
			{
				/* Patch courtesy: Gabominated */
				/* 50fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,00265C70,word,00000001", /* 00000002 */
					"patch=1,EE,00140A50,word,24020032"  /* 24020019 native global speed */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));

			}
			/* Rune - Viking Warlord (PAL) [CRC: 52638022] */
			else if (!strcmp(serial, "SLES-50335"))
			{
				/* Patch courtesy: PeterDelta */
				/* 50fps uncapped. Need EE Overclock at 180%. */
				int i;
				char *patches[] = {
					"patch=1,EE,001307AC,extended,28420001" /* 28420002 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Scarface - The World is Yours (NTSC-U) [CRC: 41F4A178] */
			else if (!strcmp(serial, "SLES-54182"))
			{
				/* 50fps uncapped. Need EE Overclock at 180%. */
				int i;
				char *patches[] = {
					"patch=1,EE,00DAFCBC,word,00000000" /* 00000001 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Spawn - Armageddon (PAL) [CRC: 8C9BF4F9] */
			else if (!strcmp(serial, "SLES-52326"))
			{
				/* Patch courtesy: PeterDelta */
				/* 50fps uncapped. Need EE Overclock at 130%. */
				int i;
				char *patches[] = {
					"patch=0,EE,00227CB0,word,24020001" /* 24020002 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Splinter Cell - Pandora Tomorrow (PAL) [CRC: 80FAC91D] */
			else if (!strcmp(serial, "SLES-52149"))
			{
				/* Patch courtesy: PeterDelta */
				/* 50fps uncapped. Need EE Overclock at 130%. */
				int i;
				char *patches[] = {
					"patch=1,EE,0018D7C8,word,24030001" /* 24030002 */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		if (!strncmp("SCUS-", serial, strlen("SCUS-")))
		{
			/* Primal (NTSC-U) [CRC: FCD89DC3] */
			if (!strcmp(serial, "SCUS-97142"))
			{
				/* 60fps uncapped */
				int i;
				char *patches[] = {
					"patch=1,EE,204874FC,word,00000001"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
	}
	
	if (hint_widescreen > 0)
	{
		if (!strncmp("SLUS-", serial, strlen("SLUS-")))
		{
			/* Alias (NTSC-U) [CRC: E3ADDC73] */
			if (!strcmp(serial, "SLUS-20673"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,00248178,word,3c013ec0", /* 3c013f00 hor fov */
					"patch=1,EE,001f3c30,word,3c013f40" /* 3c013f80 renderfix */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Need For Speed Underground 2 (NTSC-U) [CRC: F5C7B45F] */
			else if (!strcmp(serial, "SLUS-21065"))
			{
				int i;
				char *patches[] = {
					"patch=0,EE,20276E20,extended,A2C2004C" 
					/* auto enable in widescreen, boot option by default */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Return to Castle Wolfenstein: Operation Resurrection (NTSC-U) [CRC: 5F4DB1DD] */
			else if (!strcmp(serial, "SLUS-20297"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,0012f928,word,3c0142ab", /* ver FOV 1 */
					"patch=1,EE,0012f92c,word,3421e328",
					"patch=1,EE,001302dc,word,3c0142ab", /* ver FOV 2 */
					"patch=1,EE,001302e0,word,3421e328",
					"patch=1,EE,0012f6d8,word,3c0141e9", /* binoculars FOV */
					"patch=1,EE,0012f724,word,3c0142d5" /* gameplay FOV */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Ridge Racer V (NTSC-U) [CRC: 06AD9CA0] */
			else if (!strcmp(serial, "SLUS-20002"))
			{
				/* Patch courtesy: No.47 */
				int i;
				char *patches[] = {
					/* single player */
					"patch=1,EE,20332694,word,43C6C000", /* 44048000 - both fov, bumper camera */
					"patch=1,EE,203326B4,word,43C6C000", /* chase cam - 44048000 - both fov, chase camera (*) */
					"patch=1,EE,20332EC4,word,3F1D0364", /* 3EEB851F - vertical FOV */

/* split screen */
					"patch=1,EE,20332690,word,43951000", /* 43C6C000 - both FOV */
					"patch=1,EE,20332ED0,word,3F1D0364", /* 3EEB851F - vert FOV, top */
					"patch=1,EE,20332ED4,word,3F1D0364", /* 3EEB851F - vert FOV, bottom */

					/* menu */
					"patch=1,EE,20332F80,word,3F199999", /* 3F4CCCCD - horizontal FOV */
					/* (*) improved chase cam that shows the whole car as 
					 * in other Ridge Racer games, instead of only the 
					 * upper half */
					/* (*) replace 43960000 by 43C6C000 for the 
					 * original chase cam */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Rumble Roses (NTSC-U) [CRC: C1C91715] */
			else if (!strcmp(serial, "SLUS-20970"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,00103fa4,word,3c013f40",
					"patch=1,EE,00103fa8,word,44810000",
					"patch=1,EE,00103fb0,word,4600c602",
					/* Font fix */
					"patch=1,EE,00305bbc,word,3c033f53", /* 3c033f8c */
					"patch=1,EE,00305bc0,word,34633333" /* 3463cccd */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Street Fighter EX3 (NTSC-U) [CRC: 72B3802A] */
			else if (!strcmp(serial, "SLUS-201301")) 
			{
				/* Patch courtesy: paul_met */
				int i;
				char *patches[] = {
					"patch=1,EE,002e34d4,word,3c013f40",
					"patch=1,EE,002e34d8,word,44810000",
					"patch=1,EE,002e34e0,word,4600c602"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Stretch Panic (NTSC-U) [CRC: 854D5885] */
			else if (!strcmp(serial, "SLUS-20182")) 
			{
				if (hint_widescreen == 2) /* 16:10 */
				{
					/* Patch courtesy: sergx12 */
					int i;
					char *patches[] = {
						//16:10 widescreen (ultra)
						"patch=1,EE,2011FB08,extended,08047ECE", //0441000B - j $0011fb38 - Jump over game logic for SIO2MAN.IRX debug error message to prevent conflicts
						"patch=1,EE,2011FB4C,extended,08047EE0", //0441000C - j $0011fb80 - Jump over game logic for PADMAN.IRX debug error message to prevent conflicts
						"patch=1,EE,2012BFD8,extended,08047ECA", //C60F017C - j $0011fb28 - Jump to injected MIPS instructions for cutscene/playing check
						"patch=1,EE,2011FB28,extended,52600013", //0C043184 - beql s3, zero, $0011fb78 - Branch to the end of the widescreen logic if a cutscene is running (by checking if s3 register is 0)
						"patch=1,EE,2011FB2C,extended,00000000", //70003628 - nop - Nop delay slot (to avoid a "branch XXXXXXXX in delay slot!" warning in PCSX2's log)
						"patch=1,EE,2011FB30,extended,08047ED5", //0440FFF7 - j $0011fb54 - Jump to injected MIPS instructions for widescreen logic
						"patch=1,EE,2011FB54,extended,3C013FAA", //3C02006A - lui at, $3faa - Set $f31 register to 1.333333373 #1
						"patch=1,EE,2011FB58,extended,3421AAAB", //0C046690 - ori at, at, $aaab - Set $f31 register to 1.333333373 #2
						"patch=1,EE,2011FB5C,extended,4481F800", //2444B2E0 - mtc1 at, $f31 - Set $f31 register to 1.333333373 #3
						"patch=1,EE,2011FB60,extended,461F6302", //3C02006A - mul.s $f12, $f12, $f31 - Multiply $f12 by $f31 and store the result in $f12 (positive X FOV)
						"patch=1,EE,2011FB64,extended,46006347", //2444B2C0 - neg.s $f13, $f12 - Negate $f12 and store the result in $f13 (negative X FOV)
						"patch=1,EE,2011FB68,extended,3C013F8E", //70002E28 - lui at, $3f8e - Set $f31 register to 1.111111164 #1
						"patch=1,EE,2011FB6C,extended,342138E4", //0C043184 - ori at, at, $38e4 - Set $f31 register to 1.111111164 #2
						"patch=1,EE,2011FB70,extended,4481F800", //70003628 - mtc1 at, $f31 - Set $f31 register to 1.111111164 #3
						"patch=1,EE,2011FB74,extended,461F7382", //0440FFF7 - mul.s $f14, $f14, $f31 - Multiply $f14 by $f31 and store the result in $f14 (negative Y FOV)
						"patch=1,EE,2011FB78,extended,0804AFF8", //00000000 - j $0012bfe0 - Jump to 2 lines after the overwritten MIPS instruction
						"patch=1,EE,2011FB7C,extended,460073C7" //00000000 - neg.s $f15, $f14 - Negate $f14 to make it positive and store the result in $f15 (positive Y FOV)
					};
					for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
						LoadPatchesFromString(std::string(patches[i]));
				}
				else
				{
					/* Patch courtesy: sergx12 */
					int i;
					char *patches[] = {
						//16:9 widescreen
						"patch=1,EE,2011FB4C,extended,08047EE0", //0441000C - j $0011fb80 - Jump over game logic for PADMAN.IRX debug error message to prevent conflicts
						"patch=1,EE,2012BFD0,extended,08047ED5", //C60D0174 - j $0011fb54 - Jump to injected MIPS instructions for widescreen logic
						"patch=1,EE,2011FB54,extended,12600004", //3C02006A - beq s3, zero, $0011fb68 - Branch to the end of the widescreen logic if a cutscene is running (by checking if s3 register is 0)
						"patch=1,EE,2011FB58,extended,3C013FAA", //0C046690 - lui at, $3faa - Set $f31 register to 1.333333373 #1
						"patch=1,EE,2011FB5C,extended,3421AAAB", //2444B2E0 - ori at, at, $aaab - Set $f31 register to 1.333333373 #2
						"patch=1,EE,2011FB60,extended,4481F800", //3C02006A - mtc1 at, $f31 - Set $f31 register to 1.333333373 #3
						"patch=1,EE,2011FB64,extended,461F6302", //2444B2C0 - mul.s $f12, $f12, $f31 - Multiply $f12 by $f31 and store the result in $f12 (positive X FOV)
						"patch=1,EE,2011FB68,extended,0804AFF6", //70002E28 - j $0012bfd8 - Jump to 2 lines after the overwritten MIPS instruction
						"patch=1,EE,2011FB6C,extended,46006347" //0C043184 - neg.s $f13, $f12 - Negate $f12 and store the result in $f13 (negative X FOV)
					};
					for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
						LoadPatchesFromString(std::string(patches[i]));
				}
			}
			/* Tekken Tag Tournament (NTSC-U) [CRC: 67454C1E] */
			else if (!strcmp(serial, "SLUS-20001")) 
			{
				int i;
				char *patches[] = {
					"patch=0,EE,90402148,extended,0c1007f8",
					"patch=0,EE,2034b014,extended,3c013f40",
					"patch=0,EE,2034b018,extended,44810000",
					"patch=0,EE,2034b020,extended,4600c602"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Tekken 4 (NTSC-U) [CRC: 833FE0A4] */
			else if (!strcmp(serial, "SLUS-20328")) 
			{
				int i;
				char *patches[] = {
					/* EE patch */
					"patch=1,EE,002917E4,word,24020001",
					"patch=1,EE,002919E4,word,00000000",
					/* Gameplay */
					"patch=1,EE,00216EA0,word,3c013f40",
					/* Partial HUD fix */
					"patch=1,EE,001F6BF8,word,3C013F40",
					/* rfix 1 */
					"patch=1,EE,0018D408,word,3c0143d5",
					/* rfix 2 */
					"patch=1,EE,00200d84,word,3c013f40",
					"patch=1,EE,00200d88,word,44810000",
					"patch=1,EE,00200d94,word,46006303"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
			/* Tekken 5 (NTSC-U) [CRC: 652050D2] */
			else if (!strcmp(serial, "SLUS-21059")) 
			{
				int i;
				char *patches[] = {
					"patch=1,EE,E0048870,extended,01FFEF20",
					"patch=1,EE,D03E453A,extended,0100",
					"patch=1,EE,0032B448,extended,40",
					"patch=1,EE,D03E453A,extended,0000",
					"patch=1,EE,0032B448,extended,80",

					"patch=1,EE,E0052C70,extended,01FFEF20",
					"patch=1,EE,2022E8FC,extended,000D6C3A",
					"patch=1,EE,2022E50C,extended,000E743A",
					"patch=1,EE,2021C9C0,extended,3C013F40",
					"patch=1,EE,2021C9CC,extended,4481F000",
					"patch=1,EE,2021C9D0,extended,461EBDC3"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SCES-", serial, strlen("SCES-")))
		{
			/* Tekken Tag Tournament (PAL) [CRC: 0DD8941C] */
			if (!strcmp(serial, "SCES-50001")) 
			{
				int i;
				char *patches[] = {
					"patch=1,EE,0034b014,word,3c013f40",
					"patch=1,EE,0034b018,word,44810000",
					"patch=1,EE,0034b020,word,4600c602"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SLES-", serial, strlen("SLES-")))
		{
			/* Alias (PAL-M) [CRC: 83466553] */
			if (!strcmp(serial, "SLES-51821"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,00248138,word,3c013ec0", /* 3c013f00 hor fov */
					"patch=1,EE,001f3c70,word,3c013f40" /* 3c013f80 renderfix */
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SCUS-", serial, strlen("SCUS-")))
		{
			/* God of War II (NTSC-U) [CRC: 2F123FD8] */
			if (!strcmp(serial, "SCUS-97481"))
			{
				int i;
				char *patches[] = {
					/* default to widescreen at first run */
					"patch=1,EE,001E45B4,word,24040001",
					"patch=1,EE,001E45B8,word,00000000",
					"patch=0,EE,0027894C,word,3c013fe3",
					"patch=0,EE,00278950,word,34218e39"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SLPM-", serial, strlen("SLPM-")))
		{
			/* Sega Rally 2006 (NTSC-J) [CRC: B26172F0] */
			if (!strcmp(serial, "SLPM-66212"))
			{
				/* Patch courtesy: VIRGIN KLM */
				int i;
				char *patches[] = {
					"patch=1,EE,20383AF8,word,3FC71C71",
					"patch=1,EE,20356EA0,word,3FC71C71",
					"patch=1,EE,20356EC0,word,3FC71C71",
					"patch=1,EE,2038445C,word,00000001",
					"patch=1,EE,20383C3C,word,3FC00000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
		else if (!strncmp("SLPS-", serial, strlen("SLPS-")))
		{
			/* Simple 2000 Series Vol. 101 - The Oneechanpon (NTSC-J) [CRC: C5B75C7C] */
			if (!strcmp(serial, "SLPM-66212"))
			{
				int i;
				char *patches[] = {
					"patch=1,EE,20495104,extended,3F400000"
				};
				for (i = 0; i < sizeof(patches) / sizeof((patches)[0]); i++)
					LoadPatchesFromString(std::string(patches[i]));
			}
		}
	}
}
