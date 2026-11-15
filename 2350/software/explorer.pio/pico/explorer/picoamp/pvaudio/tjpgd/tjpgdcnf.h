/*----------------------------------------------*/
/* TJpgDec System Configurations R0.03          */
/* (the PicoVerse Audio Engine's: covers)       */
/*----------------------------------------------*/

#define	JD_SZBUF		512     /* stream input buffer */
#define JD_FORMAT		0       /* RGB888 */
#define	JD_USE_SCALE	1       /* 1/2, 1/4, 1/8 while decoding */
#define JD_TBLCLIP		1
#define JD_FASTDECODE	1       /* 32-bit barrel shifter */

#if JD_FASTDECODE == 0
 #define TJPGD_WORKSPACE_SIZE 3100
#elif JD_FASTDECODE == 1
 #define TJPGD_WORKSPACE_SIZE 3500
#elif JD_FASTDECODE == 2
 #define TJPGD_WORKSPACE_SIZE (3500 + 6144)
#endif
