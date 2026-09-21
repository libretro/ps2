/* The register-write dispatch table, listed once and read twice: once for
 * the handler each address goes to, once for the argument that handler is
 * called with. Both arrays are built from these entries, so an address can
 * never pick up the wrong argument. */

	VoiceParamsCore(0), /* 0x000 -> 0x180 */
		CoreParamsPair(0, REG_S_PMON),
		CoreParamsPair(0, REG_S_NON),
		CoreParamsPair(0, REG_S_VMIXL),
		CoreParamsPair(0, REG_S_VMIXEL),
		CoreParamsPair(0, REG_S_VMIXR),
		CoreParamsPair(0, REG_S_VMIXER),

		RCORE(0, REG_P_MMIX),
		RCORE(0, REG_C_ATTR),

		CoreParamsPair(0, REG_A_IRQA),
		CoreParamsPair(0, REG_S_KON),
		CoreParamsPair(0, REG_S_KOFF),
		CoreParamsPair(0, REG_A_TSA),
		CoreParamsPair(0, REG__1AC),

		RCORE(0, REG_S_ADMAS),
		REGRAW(0x1b2),

		REGRAW(0x1b4), REGRAW(0x1b6),
		REGRAW(0x1b8), REGRAW(0x1ba),
		REGRAW(0x1bc), REGRAW(0x1be),

		/* 0x1c0! */

		VoiceAddrSet(0, 0), VoiceAddrSet(0, 1), VoiceAddrSet(0, 2), VoiceAddrSet(0, 3), VoiceAddrSet(0, 4), VoiceAddrSet(0, 5),
		VoiceAddrSet(0, 6), VoiceAddrSet(0, 7), VoiceAddrSet(0, 8), VoiceAddrSet(0, 9), VoiceAddrSet(0, 10), VoiceAddrSet(0, 11),
		VoiceAddrSet(0, 12), VoiceAddrSet(0, 13), VoiceAddrSet(0, 14), VoiceAddrSet(0, 15), VoiceAddrSet(0, 16), VoiceAddrSet(0, 17),
		VoiceAddrSet(0, 18), VoiceAddrSet(0, 19), VoiceAddrSet(0, 20), VoiceAddrSet(0, 21), VoiceAddrSet(0, 22), VoiceAddrSet(0, 23),

		CoreParamsPair(0, REG_A_ESA),

		CoreParamsPair(0, R_APF1_SIZE),   /*       0x02E4		  Feedback Source A */
		CoreParamsPair(0, R_APF2_SIZE),   /*       0x02E8		  Feedback Source B */
		CoreParamsPair(0, R_SAME_L_DST),  /*    0x02EC */
		CoreParamsPair(0, R_SAME_R_DST),  /*    0x02F0 */
		CoreParamsPair(0, R_COMB1_L_SRC), /*     0x02F4 */
		CoreParamsPair(0, R_COMB1_R_SRC), /*     0x02F8 */
		CoreParamsPair(0, R_COMB2_L_SRC), /*     0x02FC */
		CoreParamsPair(0, R_COMB2_R_SRC), /*     0x0300 */
		CoreParamsPair(0, R_SAME_L_SRC),  /*     0x0304 */
		CoreParamsPair(0, R_SAME_R_SRC),  /*     0x0308 */
		CoreParamsPair(0, R_DIFF_L_DST),  /*    0x030C */
		CoreParamsPair(0, R_DIFF_R_DST),  /*    0x0310 */
		CoreParamsPair(0, R_COMB3_L_SRC), /*     0x0314 */
		CoreParamsPair(0, R_COMB3_R_SRC), /*     0x0318 */
		CoreParamsPair(0, R_COMB4_L_SRC), /*     0x031C */
		CoreParamsPair(0, R_COMB4_R_SRC), /*     0x0320 */
		CoreParamsPair(0, R_DIFF_L_SRC),  /*     0x0324 */
		CoreParamsPair(0, R_DIFF_R_SRC),  /*     0x0328 */
		CoreParamsPair(0, R_APF1_L_DST),  /*    0x032C */
		CoreParamsPair(0, R_APF1_R_DST),  /*    0x0330 */
		CoreParamsPair(0, R_APF2_L_DST),  /*    0x0334 */
		CoreParamsPair(0, R_APF2_R_DST),  /*    0x0338 */

		RCORE(0, REG_A_EEA), RNULL,

		CoreParamsPair(0, REG_S_ENDX), /*       0x0340	  End Point passed flag */
		RCORE(0, REG_P_STATX), /*      0x0344 	  Status register? */

		/*0x346 here */
		REGRAW(0x346),
		REGRAW(0x348), REGRAW(0x34A), REGRAW(0x34C), REGRAW(0x34E),
		REGRAW(0x350), REGRAW(0x352), REGRAW(0x354), REGRAW(0x356),
		REGRAW(0x358), REGRAW(0x35A), REGRAW(0x35C), REGRAW(0x35E),
		REGRAW(0x360), REGRAW(0x362), REGRAW(0x364), REGRAW(0x366),
		REGRAW(0x368), REGRAW(0x36A), REGRAW(0x36C), REGRAW(0x36E),
		REGRAW(0x370), REGRAW(0x372), REGRAW(0x374), REGRAW(0x376),
		REGRAW(0x378), REGRAW(0x37A), REGRAW(0x37C), REGRAW(0x37E),
		REGRAW(0x380), REGRAW(0x382), REGRAW(0x384), REGRAW(0x386),
		REGRAW(0x388), REGRAW(0x38A), REGRAW(0x38C), REGRAW(0x38E),
		REGRAW(0x390), REGRAW(0x392), REGRAW(0x394), REGRAW(0x396),
		REGRAW(0x398), REGRAW(0x39A), REGRAW(0x39C), REGRAW(0x39E),
		REGRAW(0x3A0), REGRAW(0x3A2), REGRAW(0x3A4), REGRAW(0x3A6),
		REGRAW(0x3A8), REGRAW(0x3AA), REGRAW(0x3AC), REGRAW(0x3AE),
		REGRAW(0x3B0), REGRAW(0x3B2), REGRAW(0x3B4), REGRAW(0x3B6),
		REGRAW(0x3B8), REGRAW(0x3BA), REGRAW(0x3BC), REGRAW(0x3BE),
		REGRAW(0x3C0), REGRAW(0x3C2), REGRAW(0x3C4), REGRAW(0x3C6),
		REGRAW(0x3C8), REGRAW(0x3CA), REGRAW(0x3CC), REGRAW(0x3CE),
		REGRAW(0x3D0), REGRAW(0x3D2), REGRAW(0x3D4), REGRAW(0x3D6),
		REGRAW(0x3D8), REGRAW(0x3DA), REGRAW(0x3DC), REGRAW(0x3DE),
		REGRAW(0x3E0), REGRAW(0x3E2), REGRAW(0x3E4), REGRAW(0x3E6),
		REGRAW(0x3E8), REGRAW(0x3EA), REGRAW(0x3EC), REGRAW(0x3EE),
		REGRAW(0x3F0), REGRAW(0x3F2), REGRAW(0x3F4), REGRAW(0x3F6),
		REGRAW(0x3F8), REGRAW(0x3FA), REGRAW(0x3FC), REGRAW(0x3FE),

		/* AND... we reached 0x400! */
		/* Last verse, same as the first: */

		VoiceParamsCore(1), /* 0x000 -> 0x180 */
		CoreParamsPair(1, REG_S_PMON),
		CoreParamsPair(1, REG_S_NON),
		CoreParamsPair(1, REG_S_VMIXL),
		CoreParamsPair(1, REG_S_VMIXEL),
		CoreParamsPair(1, REG_S_VMIXR),
		CoreParamsPair(1, REG_S_VMIXER),

		RCORE(1, REG_P_MMIX),
		RCORE(1, REG_C_ATTR),

		CoreParamsPair(1, REG_A_IRQA),
		CoreParamsPair(1, REG_S_KON),
		CoreParamsPair(1, REG_S_KOFF),
		CoreParamsPair(1, REG_A_TSA),
		CoreParamsPair(1, REG__1AC),

		RCORE(1, REG_S_ADMAS),
		REGRAW(0x5b2),

		REGRAW(0x5b4), REGRAW(0x5b6),
		REGRAW(0x5b8), REGRAW(0x5ba),
		REGRAW(0x5bc), REGRAW(0x5be),

		/* 0x1c0! */

		VoiceAddrSet(1, 0), VoiceAddrSet(1, 1), VoiceAddrSet(1, 2), VoiceAddrSet(1, 3), VoiceAddrSet(1, 4), VoiceAddrSet(1, 5),
		VoiceAddrSet(1, 6), VoiceAddrSet(1, 7), VoiceAddrSet(1, 8), VoiceAddrSet(1, 9), VoiceAddrSet(1, 10), VoiceAddrSet(1, 11),
		VoiceAddrSet(1, 12), VoiceAddrSet(1, 13), VoiceAddrSet(1, 14), VoiceAddrSet(1, 15), VoiceAddrSet(1, 16), VoiceAddrSet(1, 17),
		VoiceAddrSet(1, 18), VoiceAddrSet(1, 19), VoiceAddrSet(1, 20), VoiceAddrSet(1, 21), VoiceAddrSet(1, 22), VoiceAddrSet(1, 23),

		CoreParamsPair(1, REG_A_ESA),

		CoreParamsPair(1, R_APF1_SIZE),   /*       0x02E4		  Feedback Source A */
		CoreParamsPair(1, R_APF2_SIZE),   /*       0x02E8		  Feedback Source B */
		CoreParamsPair(1, R_SAME_L_DST),  /*    0x02EC */
		CoreParamsPair(1, R_SAME_R_DST),  /*    0x02F0 */
		CoreParamsPair(1, R_COMB1_L_SRC), /*     0x02F4 */
		CoreParamsPair(1, R_COMB1_R_SRC), /*     0x02F8 */
		CoreParamsPair(1, R_COMB2_L_SRC), /*     0x02FC */
		CoreParamsPair(1, R_COMB2_R_SRC), /*     0x0300 */
		CoreParamsPair(1, R_SAME_L_SRC),  /*     0x0304 */
		CoreParamsPair(1, R_SAME_R_SRC),  /*     0x0308 */
		CoreParamsPair(1, R_DIFF_L_DST),  /*    0x030C */
		CoreParamsPair(1, R_DIFF_R_DST),  /*    0x0310 */
		CoreParamsPair(1, R_COMB3_L_SRC), /*     0x0314 */
		CoreParamsPair(1, R_COMB3_R_SRC), /*     0x0318 */
		CoreParamsPair(1, R_COMB4_L_SRC), /*     0x031C */
		CoreParamsPair(1, R_COMB4_R_SRC), /*     0x0320 */
		CoreParamsPair(1, R_DIFF_R_SRC),  /*     0x0324 */
		CoreParamsPair(1, R_DIFF_L_SRC),  /*     0x0328 */
		CoreParamsPair(1, R_APF1_L_DST),  /*    0x032C */
		CoreParamsPair(1, R_APF1_R_DST),  /*    0x0330 */
		CoreParamsPair(1, R_APF2_L_DST),  /*    0x0334 */
		CoreParamsPair(1, R_APF2_R_DST),  /*    0x0338 */

		RCORE(1, REG_A_EEA), RNULL,

		CoreParamsPair(1, REG_S_ENDX), /*       0x0340	  End Point passed flag */
		RCORE(1, REG_P_STATX), /*      0x0344 	  Status register? */

		REGRAW(0x746),
		REGRAW(0x748), REGRAW(0x74A), REGRAW(0x74C), REGRAW(0x74E),
		REGRAW(0x750), REGRAW(0x752), REGRAW(0x754), REGRAW(0x756),
		REGRAW(0x758), REGRAW(0x75A), REGRAW(0x75C), REGRAW(0x75E),

		/* ------ ------- */

		RCEXT(0, REG_P_MVOLL),  /*     0x0760		  Master Volume Left */
		RCEXT(0, REG_P_MVOLR),  /*     0x0762		  Master Volume Right */
		RCEXT(0, REG_P_EVOLL),  /*     0x0764		  Effect Volume Left */
		RCEXT(0, REG_P_EVOLR),  /*     0x0766		  Effect Volume Right */
		RCEXT(0, REG_P_AVOLL),  /*     0x0768		  Core External Input Volume Left  (Only Core 1) */
		RCEXT(0, REG_P_AVOLR),  /*     0x076A		  Core External Input Volume Right (Only Core 1) */
		RCEXT(0, REG_P_BVOLL),  /*     0x076C 		  Sound Data Volume Left */
		RCEXT(0, REG_P_BVOLR),  /*     0x076E		  Sound Data Volume Right */
		RCEXT(0, REG_P_MVOLXL), /*     0x0770		  Current Master Volume Left */
		RCEXT(0, REG_P_MVOLXR), /*     0x0772		  Current Master Volume Right */

		RCEXT(0, R_IIR_VOL),   /*     0x0774		 IIR alpha (% used) */
		RCEXT(0, R_COMB1_VOL), /*     0x0776 */
		RCEXT(0, R_COMB2_VOL), /*     0x0778 */
		RCEXT(0, R_COMB3_VOL), /*     0x077A */
		RCEXT(0, R_COMB4_VOL), /*     0x077C */
		RCEXT(0, R_WALL_VOL),  /*     0x077E */
		RCEXT(0, R_APF1_VOL),  /*     0x0780		 feedback alpha (% used) */
		RCEXT(0, R_APF2_VOL),  /*     0x0782		 feedback */
		RCEXT(0, R_IN_COEF_L), /*     0x0784 */
		RCEXT(0, R_IN_COEF_R), /*     0x0786 */

		/* ------ ------- */

		RCEXT(1, REG_P_MVOLL),  /*     0x0788		  Master Volume Left */
		RCEXT(1, REG_P_MVOLR),  /*     0x078A		  Master Volume Right */
		RCEXT(1, REG_P_EVOLL),  /*     0x0764		  Effect Volume Left */
		RCEXT(1, REG_P_EVOLR),  /*     0x0766		  Effect Volume Right */
		RCEXT(1, REG_P_AVOLL),  /*     0x0768		  Core External Input Volume Left  (Only Core 1) */
		RCEXT(1, REG_P_AVOLR),  /*     0x076A		  Core External Input Volume Right (Only Core 1) */
		RCEXT(1, REG_P_BVOLL),  /*     0x076C		  Sound Data Volume Left */
		RCEXT(1, REG_P_BVOLR),  /*     0x076E		  Sound Data Volume Right */
		RCEXT(1, REG_P_MVOLXL), /*     0x0770		  Current Master Volume Left */
		RCEXT(1, REG_P_MVOLXR), /*     0x0772		  Current Master Volume Right */

		RCEXT(1, R_IIR_VOL),   /*     0x0774		 IIR alpha (% used) */
		RCEXT(1, R_COMB1_VOL), /*     0x0776 */
		RCEXT(1, R_COMB2_VOL), /*     0x0778 */
		RCEXT(1, R_COMB3_VOL), /*     0x077A */
		RCEXT(1, R_COMB4_VOL), /*     0x077C */
		RCEXT(1, R_WALL_VOL),  /*     0x077E */
		RCEXT(1, R_APF1_VOL),  /*     0x0780		 feedback alpha (% used) */
		RCEXT(1, R_APF2_VOL),  /*     0x0782		 feedback */
		RCEXT(1, R_IN_COEF_L), /*     0x0784 */
		RCEXT(1, R_IN_COEF_R), /*     0x0786 */

		REGRAW(0x7B0), REGRAW(0x7B2), REGRAW(0x7B4), REGRAW(0x7B6),
		REGRAW(0x7B8), REGRAW(0x7BA), REGRAW(0x7BC), REGRAW(0x7BE),

		/*  SPDIF interface */

		RSPDIF(SPDIF_OUT),     /*    0x07C0		  SPDIF Out: OFF/'PCM'/Bitstream/Bypass */
		RSPDIF(SPDIF_IRQINFO), /*    0x07C2 */
		REGRAW(0x7C4),
		RSPDIF(SPDIF_MODE),  /*    0x07C6 */
		RSPDIF(SPDIF_MEDIA), /*    0x07C8		  SPDIF Media: 'CD'/DVD */
		REGRAW(0x7CA),
		RSPDIF(SPDIF_PROTECT), /*	 0x07CC		  SPDIF Copy Protection */

		REGRAW(0x7CE),
		REGRAW(0x7D0), REGRAW(0x7D2), REGRAW(0x7D4), REGRAW(0x7D6),
		REGRAW(0x7D8), REGRAW(0x7DA), REGRAW(0x7DC), REGRAW(0x7DE),
		REGRAW(0x7E0), REGRAW(0x7E2), REGRAW(0x7E4), REGRAW(0x7E6),
		REGRAW(0x7E8), REGRAW(0x7EA), REGRAW(0x7EC), REGRAW(0x7EE),
		REGRAW(0x7F0), REGRAW(0x7F2), REGRAW(0x7F4), REGRAW(0x7F6),
		REGRAW(0x7F8), REGRAW(0x7FA), REGRAW(0x7FC), REGRAW(0x7FE),

		RNULLPTR /* should be at 0x400!  (we assert check it on startup) */

