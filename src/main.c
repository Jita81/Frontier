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
#include "fx.h"
#include "shortcut.h"
#include "oracle.h"
#include "bindings.h"
#include "ini.h"


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
int flat_fx = 0;	/* --flat-fx: no HD weapon effects (fx.c); both renderers link this */
int flat_explosions = 0;	/* --flat-explosions: the base explosions, other HD weapon effects kept */
int flat_lights = 0;	/* --flat-lights: no halo round the game's lights (gl.c) */
int flat_lasers = 0;	/* --flat-lasers: every laser one plain ribbon, as before bolts and hot cores */
int flat_hud_hits = 0;	/* --flat-hud-hits: one red hit flash, no critical tint (hddamage.c) */
int flat_exhaust = 0;	/* --flat-exhaust: the game's flat engine plumes (gl.c) */

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
       Main_Quit (0);
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
       if (!bindings_mouse_event (event.button.button, 1))
         Input_MousePress (event.button.button);
       break;
    case SDL_MOUSEBUTTONUP:
       if (!bindings_mouse_event (event.button.button, 0))
         Input_MouseRelease (event.button.button);
       break;
    case SDL_KEYDOWN:
       if (event.key.keysym.sym == SDLK_h && (event.key.keysym.mod & KMOD_CTRL)
           && (event.key.keysym.mod & KMOD_SHIFT)) {
          /* hull look, 0 to 5, then off (--flat-hulls), and round again
           * (see --hull-style) */
          if (flat_hulls) flat_hulls = 0, hull_style = 0;
          else if (hull_style >= 5) flat_hulls = 1;
          else hull_style++;
          if (flat_hulls) fprintf (stderr, "hull style off, flat\n");
          else fprintf (stderr, "hull style %d\n", hull_style);
          break;
       }
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
  Screen_Stalled ();
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
               "  --flat-sky            The game's flat Milky Way band (no star clouds).\n"
               "  --flat-planets        Hard light zones and flat atmosphere rings.\n"
               "  --flat-stars          No corona round the stars.\n"
               "  --flat-lights         The game's lights without their soft halo.\n"
               "  --pixel-text          Keep the original bitmap font (no HD text;\n"
               "                        FE2_HD_FONT=<PostScript name> picks the font).\n"
               "  --pixel-art           Keep the game's own bitmaps (no HD stills).\n"
               "  --pixel-faces         Keep the game's own faces (no HD portraits).\n"
               "  --classic-ui          Keep the game's own flat 2D screens.\n"
               "  --flat-fx             Flat lasers, missiles and explosions (no HD effects).\n"
               "  --flat-explosions     Flat explosions only (HD lasers and missiles kept).\n"
               "  --flat-lasers         Every laser one steady ribbon (no bolts or hot cores).\n"
               "  --flat-hud-hits       One red flash for every hit, no critical hull tint.\n"
               "  --flat-exhaust        The game's flat engine plumes (no HD exhaust).\n"
               "  --hull-style N        Hull look, 0 plating, 1 big texture, 2 panels,\n"
               "                        3 bump and weathering, 4 per model, 5 a material\n"
               "                        per class of ship (default).\n"
               "                        Ctrl+Shift+H steps through them and off.\n"
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
	  else if (!strcmp(argv[i], "--flat-sky"))
		flat_sky = 1;
	  else if (!strcmp(argv[i], "--flat-planets"))
		flat_planets = 1;
	  else if (!strcmp(argv[i], "--flat-stars"))
		flat_stars = 1;
	  else if (!strcmp(argv[i], "--flat-lights"))
		flat_lights = 1;
	  else if (!strcmp(argv[i], "--pixel-text"))
		pixel_text = 1;
	  else if (!strcmp(argv[i], "--pixel-art"))
		pixel_art = 1;
	  else if (!strcmp(argv[i], "--pixel-faces"))
		pixel_faces = 1;
	  else if (!strcmp(argv[i], "--classic-ui"))
		classic_ui = 1;
	  else if (!strcmp(argv[i], "--flat-fx"))
		flat_fx = 1;
	  else if (!strcmp(argv[i], "--flat-explosions"))
		flat_explosions = 1;
	  else if (!strcmp(argv[i], "--flat-lasers"))
		flat_lasers = 1;
	  else if (!strcmp(argv[i], "--flat-hud-hits"))
		flat_hud_hits = 1;
	  else if (!strcmp(argv[i], "--flat-exhaust"))
		flat_exhaust = 1;
	  else if (!strcmp(argv[i], "--hull-style") && i + 1 < argc) {
		hull_style = atoi (argv[++i]);
		if (hull_style < 0 || hull_style > 5) hull_style = 5;
	  }
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
    Main_Quit(-1);
  }

  /* --size: at least 640x480, at most the desktop less the menu bar, title
   * bar and dock (128 points, as gl.c's pick_window_size); too big shrinks
   * keeping its shape */
  if (screen_w > 0 && screen_h > 0)
  {
    const SDL_VideoInfo *vi = SDL_GetVideoInfo();
    int w = screen_w, h = screen_h, mw = vi ? vi->current_w : 0, mh = vi ? vi->current_h - 128 : 0;
    if (mw > 0 && mh > 0 && (w > mw || h > mh))
    {
      double s = (double) mw / w < (double) mh / h ? (double) mw / w : (double) mh / h;
      w = (int) (w * s);
      h = (int) (h * s);
    }
    if (w < 640) w = 640;
    if (h < 480) h = 480;
    if (mw >= 640 && w > mw) w = mw;
    if (mh >= 480 && h > mh) h = mh;
    if (w != screen_w || h != screen_h)
      fprintf(stderr, "--size %dx%d clamped to %dx%d (640x480 to %dx%d)\n", screen_w, screen_h, w, h, mw, mh);
    screen_w = w;
    screen_h = h;
  }
  
  Screen_Init();
  Init680x0();                  /* Init CPU emulation */
  Audio_Init();
  Keymap_Init();

  if(bQuitProgram)
    Main_Quit(-2);
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
  Every way out (the window's close button, Ctrl-Q, the end of main, a
  failed start): the VBL thread is stopped and waited for and the audio
  closed before SDL_Quit, so no thread of ours runs on while SDL and the
  C library shut down.  A second call while quitting goes straight out.
*/
static volatile sig_atomic_t quitting;

void Main_Quit(int code)
{
  if (quitting)
    _exit(code);
  quitting = 1;
  bQuitProgram = TRUE;
  if (vbl_thread) {
    vbl_quit = 1;
    SDL_WaitThread (vbl_thread, NULL);
    vbl_thread = NULL;
  }
  Keymap_UnInit();
  Audio_UnInit();
  Screen_UnInit();
  SDL_Quit();
  exit(code);
}

/* frontier.ini keeps one thing: the renderer Ctrl-E last chose.  It is
 * read before the command line, so --old-renderer still wins; an oracle
 * run neither reads nor writes it. */
#define SETTINGS_FILE "frontier.ini"

static void Main_LoadSettings(void)
{
  struct INI *ini;
  const char *name, *key, *val;
  size_t len, lkey, lval;

  if (oracle.active || !(ini = ini_open(SETTINGS_FILE)))
    return;
  while (ini_next_section(ini, &name, &len) > 0)
    while (ini_read_pair(ini, &key, &lkey, &val, &lval) > 0)
      if (lkey == 8 && !strncasecmp(key, "RENDERER", 8) && lval == 3 && !strncasecmp(val, "old", 3))
        use_renderer = R_OLD;
  ini_close(ini);
}

void Main_SaveSettings(void)
{
  FILE *f;

  if (oracle.active || !(f = fopen(SETTINGS_FILE ".tmp", "w")))
    return;
  fprintf(f, "; Frontier settings, written by the game (Ctrl-E). Delete it for the defaults.\n"
          "[Display]\nRENDERER=%s\n", use_renderer == R_OLD ? "old" : "hd");
  if (fclose(f) != 0 || rename(SETTINGS_FILE ".tmp", SETTINGS_FILE) != 0)
    perror(SETTINGS_FILE);
}

void sig_handler (int signum)
{
	if (quitting)		/* a late signal while shutting down */
		_exit (0);
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

  Main_LoadSettings();

  /* Check for any passed parameters */
  Main_ReadParameters(argc, argv);

  /* controls.ini (never with an oracle flag) */
  bindings_init();

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
  Main_Quit(0);

  return(0);
}


