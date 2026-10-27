/*
  Hatari - main.c

  This file is distributed under the GNU Public License, version 2 or at
  your option any later version. Read the file gpl.txt for details.

  Main initialization and event handling routines.
*/

#include <time.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#include <signal.h>

#include <SDL.h>
#ifdef __APPLE__
#include <pthread.h>
#include <sys/qos.h>
#endif

#include "main.h"
#include "audio.h"
#include "../m68000.h"
#include "hostcall.h"
#include "input.h"
#include "joystick.h"
#include "keymap.h"
#include "screen.h"
#include "shortcut.h"
#include "oracle.h"


#define FORCE_WORKING_DIR                 /* Set default directory to cwd */


BOOL bQuitProgram=FALSE;                  /* Flag to quit program cleanly */
BOOL bUseFullscreen=FALSE;
BOOL bEmulationActive=TRUE;               /* Run emulation when started */
BOOL bAppActive = FALSE;
char szBootDiscImage[MAX_FILENAME_LENGTH] = { "" };

char szWorkingDir[MAX_FILENAME_LENGTH] = { "" };
char szCurrentDir[MAX_FILENAME_LENGTH] = { "" };

extern enum RENDERERS use_renderer;

int delta_x, delta_y, abs_delta_x, abs_delta_y;

/*-----------------------------------------------------------------------*/
/*
  Error handler
*/
void Main_SysError(char *Error,char *Title)
{
  fprintf(stderr,"%s : %s\n",Title,Error);
}


/*-----------------------------------------------------------------------*/
/*
  Bring up message(handles full-screen as well as Window)
*/
int Main_Message(char *lpText, char *lpCaption/*,unsigned int uType*/)
{
  int Ret=0;

  /* Show message */
  fprintf(stderr,"%s: %s\n", lpCaption, lpText);

  return(Ret);
}


/*-----------------------------------------------------------------------*/
/*
  Pause emulation, stop sound
*/
void Main_PauseEmulation(void)
{
  if( bEmulationActive )
  {
    Audio_EnableAudio(FALSE);
    bEmulationActive = FALSE;
  }
}

/*-----------------------------------------------------------------------*/
/*
  Start emulation
*/
void Main_UnPauseEmulation(void)
{
  if( !bEmulationActive )
  {
    Audio_EnableAudio(1);
    bEmulationActive = TRUE;
  }
}

/* ----------------------------------------------------------------------- */
/*
  Message handler
  Here we process the SDL events (keyboard, mouse, ...) and map it to
  Atari IKBD events.
*/

void Main_EventHandler()
{
  SDL_Event event;
  int new_abs_x, new_abs_y;

  SDL_JoystickUpdate();

  while (SDL_PollEvent (&event))
   switch( event.type )
   {
    case SDL_QUIT:
       bQuitProgram = TRUE;
       SDL_Quit ();
       exit (0);
       break;
    case SDL_MOUSEMOTION:               /* Read/Update internal mouse position */
       input.motion_x += event.motion.xrel;
       input.motion_y += event.motion.yrel;
       input.abs_x = event.motion.x;
       input.abs_y = event.motion.y;
       break;
	case SDL_JOYAXISMOTION:
	   if (event.jaxis.axis <= 1)
		   joystick_motion(event.jaxis.axis, event.jaxis.value);
	   break;
	case SDL_JOYBUTTONDOWN:
	   Keymap_JoystickUpDown(event.jbutton.button, 1);
	   break;
	case SDL_JOYBUTTONUP:
	   Keymap_JoystickUpDown(event.jbutton.button, 0);
	   break;
	case SDL_JOYHATMOTION:
	   if (event.jhat.value == SDL_HAT_UP)
		   Keymap_JoystickUpDown(12, 1);
	   else if (event.jhat.value == SDL_HAT_DOWN)
		   Keymap_JoystickUpDown(13, 1);
	   else if (event.jhat.value == SDL_HAT_LEFT)
		   Keymap_JoystickUpDown(14, 1);
	   else if (event.jhat.value == SDL_HAT_RIGHT)
		   Keymap_JoystickUpDown(15, 1);
	   else {
		   int i;
		   for (i = 12; i < 16; i++)
			   Keymap_JoystickUpDown(i, 0);
	   }
	   break;
    case SDL_MOUSEBUTTONDOWN:
       Input_MousePress (event.button.button);
       break;
    case SDL_MOUSEBUTTONUP:
       Input_MouseRelease (event.button.button);
       break;
    case SDL_KEYDOWN:
       Keymap_KeyDown(&event.key.keysym);
       break;
    case SDL_KEYUP:
       Keymap_KeyUp(&event.key.keysym);
       break;
   }

  input.motion_x += delta_x;
  input.motion_y += delta_y;

  new_abs_x = input.abs_x + abs_delta_x;
  if (new_abs_x > 0 && new_abs_x < screen_w)
	  input.abs_x = new_abs_x;

  new_abs_y = input.abs_y + abs_delta_y;
  if (new_abs_y > 0 && new_abs_y < screen_h)
	  input.abs_y = new_abs_y;

  Input_Update ();
}


/*-----------------------------------------------------------------------*/
/*
  Check for any passed parameters
*/
void Main_ReadParameters(int argc, char *argv[])
{
  int i;

  /* Scan for any which we can use */
  for(i=1; i<argc; i++)
  {
    if (strlen(argv[i])>0)
    {
      if (!strcmp(argv[i],"--help") || !strcmp(argv[i],"-h"))
      {
        printf("Usage:\n frontier [options]\n"
               "Where options are:\n"
               "  --help or -h          Print this help text and exit.\n"
               "  --fullscreen or -f    Try to use fullscreen mode.\n"
               "  --nosound             Disable sound (faster!).\n"
               "  --size w h            Start at specified window size.\n"
			   "  --old-renderer        Start with the old renderer.\n"
               "  --flat-hulls          Flat ship faces, no hull plating.\n"
               "  --pixel-text          Keep the original bitmap font (no HD text;\n"
               "                        FE2_HD_FONT=<PostScript name> picks the font).\n"
               "  --pixel-art           Keep the game's own bitmaps (no HD stills).\n"
               "  --pixel-faces         Keep the game's own faces (no HD portraits).\n"
               "  --classic-ui          Keep the game's own flat 2D screens.\n"
              );
        exit(0);
      }
      else if (!strcmp(argv[i],"--fullscreen") || !strcmp(argv[i],"-f"))
      {
        bUseFullscreen=TRUE;
      }
      else if ( !strcmp(argv[i],"--nosound") )
      {
        bDisableSound=TRUE;
      }
	  else if (!strcmp(argv[i], "--old-renderer"))
		use_renderer = R_OLD;
	  else if (!strcmp(argv[i], "--flat-hulls"))
		flat_hulls = 1;
	  else if (!strcmp(argv[i], "--pixel-text"))
		pixel_text = 1;
	  else if (!strcmp(argv[i], "--pixel-art"))
		pixel_art = 1;
	  else if (!strcmp(argv[i], "--pixel-faces"))
		pixel_faces = 1;
	  else if (!strcmp(argv[i], "--classic-ui"))
		classic_ui = 1;
      else if ( !strcmp(argv[i],"--size") )
      {
	screen_h = 0;
	if (++i < argc)	screen_w = atoi (argv[i]);
	if (++i < argc)	screen_h = atoi (argv[i]);
	/* fe2 likes 1.6 aspect ratio until i fix the mouse position
	 * to 3d object position code... */
	if (screen_h == 0) screen_h = 5*screen_w/8;
      }
      else
      {
	      /* some time make it possible to read alternative
	       * names for fe2.bin from command line */
	      fprintf(stderr,"Illegal parameter: %s\n",argv[i]);
      }
    }
  }
}


/*-----------------------------------------------------------------------*/
/*
  Initialise emulation
*/
void Main_Init(void)
{
  /* Init SDL's video subsystem. Note: Audio and joystick subsystems
     will be initialized later (failures there are not fatal). */
  if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_JOYSTICK) < 0)
  {
    fprintf(stderr, "Could not initialize the SDL library:\n %s\n", SDL_GetError() );
    exit(-1);
  }
  
  Screen_Init();
  Init680x0();                  /* Init CPU emulation */
  Audio_Init();
  Keymap_Init();

  if(bQuitProgram)
  {
    SDL_Quit();
    exit(-2);
  }
}


/* The VBL, 50 a second as on a PAL ST or Amiga.  The game's clock is counted
 * in VBLs (its handler adds a fixed step per VBL at 1x), so 1x is real time
 * only if 50 VBLs really arrive each second.  SDL's timer cannot keep that
 * rate: under sdl12-compat it fires on a coarse ~10 ms grid, so a callback
 * asking for 20 ms runs every 25-35 ms and the VBL ran at 30-40 a second.
 *
 * So a thread of its own keeps the deadlines on SDL_GetTicks' clock, sleeping
 * with SDL_Delay, and flags one VBL per deadline, within a few ms of it (on
 * macOS at user-interactive QoS, so a busy machine does not starve it).  It
 * never flags two at once: the game ends each frame by waiting for one VBL
 * (L41bc4_LimitFrameRate), and two together let the frame after the wait take
 * both steps of time and the next frame none, so that frame redraws nothing
 * that tracks the time (the clock) and shows the other buffer's old picture,
 * and motion jerks.  If it falls behind it catches up one VBL every VBL_GAP
 * ms.  A backlog longer than VBL_MAX_BACKLOG VBLs (5 s) means the host was
 * stopped (sleep, a debugger): that gap is dropped, as if the machine had been
 * switched off.  Not started under the oracle, which steps VBLs itself. */
#define VBL_MS			20
#define VBL_GAP			5
#define VBL_MAX_BACKLOG		250
static SDL_Thread *vbl_thread;
static volatile int vbl_quit;

static int vbl_run (void *unused)
{
	Uint32 next = SDL_GetTicks () + VBL_MS;

	(void) unused;
#ifdef __APPLE__
	pthread_set_qos_class_self_np (QOS_CLASS_USER_INTERACTIVE, 0);
#endif
	while (!vbl_quit) {
		Uint32 now = SDL_GetTicks ();
		Sint32 wait = (Sint32) (next - now);

		if (wait > 0) {
			SDL_Delay (wait);
			continue;
		}
		if (-wait >= VBL_MS * VBL_MAX_BACKLOG)
			next = now;
		next += VBL_MS;
		FlagException (0);
		SDL_Delay (VBL_GAP);
	}
	return 0;
}


/*-----------------------------------------------------------------------*/
/*
  Un-Initialise emulation
*/
void Main_UnInit(void)
{
  if (vbl_thread) {
    vbl_quit = 1;
    SDL_WaitThread (vbl_thread, NULL);
  }
  Keymap_UnInit();
  Audio_UnInit();
  Screen_UnInit();

  /* SDL uninit: */
  SDL_Quit();
}

void sig_handler (int signum)
{
	if (signum == SIGSEGV) {
		printf ("Segfault! All is lost! Abandon ship!\n");
		Call_DumpDebug ();
		abort ();
	}
}

/*-----------------------------------------------------------------------*/
/*
  Main
*/
int main(int argc, char *argv[])
{
  signal (SIGSEGV, sig_handler);
	
  /* Generate random seed */
  srand( time(NULL) );

  /* Oracle flags (docs/07); strips them, does nothing when none given */
  oracle_parse_args(&argc, argv);

  /* Check for any passed parameters */
  Main_ReadParameters(argc, argv);

  /* Init emulator system */
  Main_Init();

  /* Switch immediately to fullscreen if user wants to */
  if( bUseFullscreen )
    Screen_ToggleFullScreen();

  oracle_install();             /* no-op unless an oracle flag was given */

  if (!oracle_lockstep())
    vbl_thread = SDL_CreateThread (vbl_run, NULL);
  
  /* Run emulation */
  Main_UnPauseEmulation();
  Start680x0();                 /* Start emulation */

  /* Un-init emulation system */
  Main_UnInit();

  return(0);
}


