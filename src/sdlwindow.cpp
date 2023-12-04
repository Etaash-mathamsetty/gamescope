// For the nested case, reads input from the SDL window and send to wayland

#include <X11/Xlib.h>
#include <thread>
#include <mutex>
#include <string>
#include <optional>

#include <linux/input-event-codes.h>
#include <signal.h>

#include "SDL_clipboard.h"
#include "SDL_events.h"
#include "main.hpp"
#include "wlserver.hpp"
#include "sdlwindow.hpp"
#include "rendervulkan.hpp"
#include "steamcompmgr.hpp"

#include "sdlscancodetable.hpp"

#define DEFAULT_TITLE "gamescope"

static bool g_bSDLInitOK = false;
static std::mutex g_SDLInitLock;

static bool g_bWindowShown = false;

static int g_nOldNestedRefresh = 0;
static bool g_bWindowFocused = true;

static int g_nOutputWidthPts = 0;
static int g_nOutputHeightPts = 0;


extern bool steamMode;
extern bool g_bFirstFrame;

SDL_Window *g_SDLWindow;
std::vector<SDL_Window *> g_SDLWindows;

enum UserEvents
{
	USER_EVENT_TITLE,
	USER_EVENT_VISIBLE,
	USER_EVENT_GRAB,
	USER_EVENT_CURSOR,

	USER_EVENT_COUNT
};

static uint32_t g_unSDLUserEventID;

static std::mutex g_SDLWindowTitleLock;
static std::shared_ptr<std::string> g_SDLWindowTitle;
static std::shared_ptr<std::vector<uint32_t>> g_SDLWindowIcon;
static bool g_bUpdateSDLWindowTitle = false;
static bool g_bUpdateSDLWindowIcon = false;

struct SDLPendingCursor
{
	uint32_t width, height, xhot, yhot;
	std::shared_ptr<std::vector<uint32_t>> data;
};
static std::mutex g_SDLCursorLock;
static SDLPendingCursor g_SDLPendingCursorData;
static bool g_bUpdateSDLCursor = false;

static void set_gamescope_selections();

//-----------------------------------------------------------------------------
// Purpose: Convert from the remote scancode to a Linux event keycode
//-----------------------------------------------------------------------------
static inline uint32_t SDLScancodeToLinuxKey( uint32_t nScancode )
{
	if ( nScancode < sizeof( s_ScancodeTable ) / sizeof( s_ScancodeTable[0] ) )
	{
		return s_ScancodeTable[ nScancode ];
	}
	return KEY_RESERVED;
}

static inline int SDLButtonToLinuxButton( int SDLButton )
{
	switch ( SDLButton )
	{
		case SDL_BUTTON_LEFT: return BTN_LEFT;
		case SDL_BUTTON_MIDDLE: return BTN_MIDDLE;
		case SDL_BUTTON_RIGHT: return BTN_RIGHT;
		case SDL_BUTTON_X1: return BTN_SIDE;
		case SDL_BUTTON_X2: return BTN_EXTRA;
		default: return 0;
	}
}

void updateOutputRefresh( void )
{
	SDL_DisplayMode mode = { SDL_PIXELFORMAT_UNKNOWN, 0, 0, 0, 0 };
	int refresh_rate = INT32_MAX;

	for(SDL_Window* display : g_SDLWindows)
	{
		if ( SDL_GetWindowDisplayMode( display, &mode ) == 0)
		{
			refresh_rate = std::min( refresh_rate, mode.refresh_rate );
		}
	}

	g_nOutputRefresh = refresh_rate;
}

void updateOutputResolution( void )
{
		// Update g_nOutputWidthPts.

		int lowest_x = INT32_MAX;
		int lowest_y = INT32_MAX;
		int highest_x = -INT32_MAX;
		int highest_y = -INT32_MAX;
	#if SDL_VERSION_ATLEAST(2, 26, 0)
		int highest_x_px = -INT32_MAX;
		int highest_y_px = -INT32_MAX;
	#endif

		for(int i = 0; i < g_SDLWindows.size(); i++)
		{
			SDL_Window* display = g_SDLWindows[i];
			int x, y;
			int width, height;
			SDL_GetWindowPosition( display, &x, &y );
			SDL_GetWindowSize( display, &width, &height);

			if(i < g_outputs.size())
			{
				g_outputs[i].width = width;
				g_outputs[i].height = height;
			}

			if ( x < lowest_x )
				lowest_x = x;
			if ( y < lowest_y )
				lowest_y = y;

			if ( (x + width) > highest_x )
				highest_x = x + width;
			if ( (y + height) > highest_y )
				highest_y = y + height;

		#if SDL_VERSION_ATLEAST(2, 26, 0)
			SDL_GetWindowSizeInPixels( display, &width, &height );

			if ( (x + width) > highest_x_px )
				highest_x_px = x + width;
			if ( (y + height) > highest_y_px )
				highest_y_px = y + height;

			if(i < g_outputs.size())
			{
				g_outputs[i].width = width;
				g_outputs[i].height = height;
			}
		#endif
		}

		g_nOutputWidthPts = highest_x - lowest_x;
		g_nOutputHeightPts = highest_y - lowest_y;

	#if SDL_VERSION_ATLEAST(2, 26, 0)
		g_nOutputWidth = highest_x_px - lowest_x;
		g_nOutputHeight = highest_y_px - lowest_y;
	#endif
}

extern bool g_bForceRelativeMouse;

static std::string gamescope_str = DEFAULT_TITLE;

void inputSDLThreadRun( void )
{
	pthread_setname_np( pthread_self(), "gamescope-sdl" );

	SDL_Event event;
	uint32_t key;
	bool bRelativeMouse = false;

	g_unSDLUserEventID = SDL_RegisterEvents( USER_EVENT_COUNT );

	uint32_t nSDLWindowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_ALLOW_HIGHDPI;

	if ( g_bBorderlessOutputWindow == true )
	{
		nSDLWindowFlags |= SDL_WINDOW_BORDERLESS;
	}

	if ( g_bFullscreen == true || g_bMultiDisplay == true)
	{
		nSDLWindowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	}

	if ( g_bGrabbed == true )
	{
		nSDLWindowFlags |= SDL_WINDOW_KEYBOARD_GRABBED;
	}

	int num_displays = 1;

	if(g_bMultiDisplay == true)
		num_displays = SDL_GetNumVideoDisplays();

	fprintf(stderr, "SDL: num_displays: %d %d\n", num_displays, g_bMultiDisplay);

	for(int i = 0; i < num_displays; i++)
	{
		g_SDLWindows.push_back(SDL_CreateWindow(DEFAULT_TITLE,
							SDL_WINDOWPOS_UNDEFINED_DISPLAY(i),
							SDL_WINDOWPOS_UNDEFINED_DISPLAY(i),
							g_nOutputWidth,
							g_nOutputHeight,
							nSDLWindowFlags));

		if ( g_SDLWindows[i] == nullptr )
		{
			fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
			g_SDLInitLock.unlock();
			return;
		}
	}

	updateOutputResolution();

	if ( g_bForceRelativeMouse )
	{
		SDL_SetRelativeMouseMode( SDL_TRUE );
		bRelativeMouse = true;
	}

	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

	g_nOldNestedRefresh = g_nNestedRefresh;

	g_bSDLInitOK = true;
	g_SDLInitLock.unlock();

	static uint32_t fake_timestamp = 0;
	SDL_Surface *cursor_surface = nullptr;
	SDL_Surface *icon_surface = nullptr;
	SDL_Cursor *cursor = nullptr;

	while( SDL_WaitEvent( &event ) )
	{
		fake_timestamp++;

		switch( event.type )
		{
			case SDL_CLIPBOARDUPDATE:
				set_gamescope_selections();
				break;
			case SDL_MOUSEMOTION:
				if ( bRelativeMouse )
				{
					if ( g_bWindowFocused )
					{
						wlserver_lock();
						wlserver_mousemotion( event.motion.xrel, event.motion.yrel, fake_timestamp );
						wlserver_unlock();
					}
				}
				else
				{
					wlserver_lock();
					wlserver_touchmotion(
						event.motion.x / float(g_nOutputWidthPts),
						event.motion.y / float(g_nOutputHeightPts),
						0,
						fake_timestamp );
					wlserver_unlock();
				}
				break;
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP:
				wlserver_lock();
				wlserver_mousebutton( SDLButtonToLinuxButton( event.button.button ),
									  event.button.state == SDL_PRESSED,
									  fake_timestamp );
				wlserver_unlock();
				break;
			case SDL_MOUSEWHEEL:
				wlserver_lock();
				wlserver_mousewheel( -event.wheel.x, -event.wheel.y, fake_timestamp );
				wlserver_unlock();
				break;
			case SDL_FINGERMOTION:
				wlserver_lock();
				wlserver_touchmotion( event.tfinger.x, event.tfinger.y, event.tfinger.fingerId, fake_timestamp );
				wlserver_unlock();
				break;
			case SDL_FINGERDOWN:
				wlserver_lock();
				wlserver_touchdown( event.tfinger.x, event.tfinger.y, event.tfinger.fingerId, fake_timestamp );
				wlserver_unlock();
				break;
			case SDL_FINGERUP:
				wlserver_lock();
				wlserver_touchup( event.tfinger.fingerId, fake_timestamp );
				wlserver_unlock();
				break;
			case SDL_KEYDOWN:
				// If this keydown event is super + one of the shortcut keys, consume the keydown event, since the corresponding keyup
				// event will be consumed by the next case statement when the user releases the key
				if ( event.key.keysym.mod & KMOD_LGUI )
				{
					key = SDLScancodeToLinuxKey( event.key.keysym.scancode );
					const uint32_t shortcutKeys[] = {KEY_F, KEY_N, KEY_B, KEY_U, KEY_Y, KEY_I, KEY_O, KEY_S, KEY_G};
					const bool isShortcutKey = std::find(std::begin(shortcutKeys), std::end(shortcutKeys), key) != std::end(shortcutKeys);
					if ( isShortcutKey )
					{
						break;
					}
				}
			[[fallthrough]];
			case SDL_KEYUP:
				key = SDLScancodeToLinuxKey( event.key.keysym.scancode );

				if ( event.type == SDL_KEYUP && ( event.key.keysym.mod & KMOD_LGUI ) )
				{
					bool handled = true;
					switch ( key )
					{
						case KEY_F:
							g_bFullscreen = !g_bFullscreen;
							for(SDL_Window* window : g_SDLWindows)
								SDL_SetWindowFullscreen( window, g_bFullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0 );
							break;
						case KEY_N:
							g_wantedUpscaleFilter = GamescopeUpscaleFilter::PIXEL;
							break;
						case KEY_B:
							g_wantedUpscaleFilter = GamescopeUpscaleFilter::LINEAR;
							break;
						case KEY_U:
							g_wantedUpscaleFilter = (g_wantedUpscaleFilter == GamescopeUpscaleFilter::FSR) ?
								GamescopeUpscaleFilter::LINEAR : GamescopeUpscaleFilter::FSR;
							break;
						case KEY_Y:
							g_wantedUpscaleFilter = (g_wantedUpscaleFilter == GamescopeUpscaleFilter::NIS) ?
								GamescopeUpscaleFilter::LINEAR : GamescopeUpscaleFilter::NIS;
							break;
						case KEY_I:
							g_upscaleFilterSharpness = std::min(20, g_upscaleFilterSharpness + 1);
							break;
						case KEY_O:
							g_upscaleFilterSharpness = std::max(0, g_upscaleFilterSharpness - 1);
							break;
						case KEY_S:
							take_screenshot();
							break;
						case KEY_G:
							g_bGrabbed = !g_bGrabbed;
							for(SDL_Window* window : g_SDLWindows)
							{
								SDL_SetWindowKeyboardGrab( window, g_bGrabbed ? SDL_TRUE : SDL_FALSE );
							}
							g_bUpdateSDLWindowTitle = true;

							SDL_Event event;
							event.type = g_unSDLUserEventID + USER_EVENT_TITLE;
							SDL_PushEvent( &event );
							break;
						default:
							handled = false;
					}
					if ( handled )
					{
						break;
					}
				}

				// On Wayland, clients handle key repetition
				if ( event.key.repeat )
					break;

				wlserver_lock();
				wlserver_key( key, event.type == SDL_KEYDOWN, fake_timestamp );
				wlserver_unlock();
				break;
			case SDL_WINDOWEVENT:
				switch( event.window.event )
				{
					case SDL_WINDOWEVENT_CLOSE:
						raise( SIGTERM );
						break;
					default:
						break;
					case SDL_WINDOWEVENT_MOVED:
					case SDL_WINDOWEVENT_SHOWN:
						updateOutputRefresh();
						break;
					case SDL_WINDOWEVENT_SIZE_CHANGED:
						updateOutputResolution();
						updateOutputRefresh();
						break;
					case SDL_WINDOWEVENT_FOCUS_LOST:
						g_nNestedRefresh = g_nNestedUnfocusedRefresh;
						g_bWindowFocused = false;
						break;
					case SDL_WINDOWEVENT_FOCUS_GAINED:
						g_nNestedRefresh = g_nOldNestedRefresh;
						g_bWindowFocused = true;
						break;
					case SDL_WINDOWEVENT_EXPOSED:
						force_repaint();
						break;
				}
				break;
			default:
				if ( event.type == g_unSDLUserEventID + USER_EVENT_TITLE )
				{
					g_SDLWindowTitleLock.lock();
					if ( g_bUpdateSDLWindowTitle )
					{
						std::string tmp_title;

						const std::string *window_title = g_SDLWindowTitle.get();
						if (!window_title)
							window_title = &gamescope_str;

						g_bUpdateSDLWindowTitle = false;
						if ( g_bGrabbed )
						{
							tmp_title = *window_title;
							tmp_title += " (grabbed)";

							window_title = &tmp_title;
						}
						for(SDL_Window* window : g_SDLWindows)
						{
							SDL_SetWindowTitle( window, window_title->c_str() );
						}
					}

					if ( g_bUpdateSDLWindowIcon )
					{
						if ( icon_surface )
						{
							SDL_FreeSurface( icon_surface );
							icon_surface = nullptr;
						}

						if ( g_SDLWindowIcon && g_SDLWindowIcon->size() >= 3 )
						{
							const uint32_t width = (*g_SDLWindowIcon)[0];
    	   					const uint32_t height = (*g_SDLWindowIcon)[1];

							icon_surface = SDL_CreateRGBSurfaceFrom(
								&(*g_SDLWindowIcon)[2],
								width, height,
								32, width * sizeof(uint32_t),
								0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
						}

						for(SDL_Window* window : g_SDLWindows)
						{
							SDL_SetWindowIcon( window, icon_surface );
						}
					}
					g_SDLWindowTitleLock.unlock();
				}
				if ( event.type == g_unSDLUserEventID + USER_EVENT_VISIBLE )
				{
					bool should_show = !!event.user.code;

					// If we are Steam Mode in nested, show the window
					// whenever we have had a first frame to match
					// what we do in embedded with Steam for testing
					// held commits, etc.
					if ( steamMode )
						should_show |= !g_bFirstFrame;

					if ( g_bWindowShown != should_show )
					{
						g_bWindowShown = should_show;

						if ( g_bWindowShown )
						{
							for(SDL_Window* window : g_SDLWindows)
								SDL_ShowWindow( window );
						}
						else
						{
							for(SDL_Window* window : g_SDLWindows)
								SDL_HideWindow( window );
						}
					}
				}
				if ( event.type == g_unSDLUserEventID + USER_EVENT_GRAB )
				{
					bool grab = !!event.user.code;
					if ( grab != bRelativeMouse )
					{
						SDL_SetRelativeMouseMode( grab ? SDL_TRUE : SDL_FALSE );
						bRelativeMouse = grab;
					}
				}
				if ( event.type == g_unSDLUserEventID + USER_EVENT_CURSOR )
				{
					std::unique_lock lock(g_SDLCursorLock);
					if ( g_bUpdateSDLCursor )
					{
						if (cursor_surface)
							SDL_FreeSurface(cursor_surface);

						cursor_surface = SDL_CreateRGBSurfaceFrom(
							g_SDLPendingCursorData.data->data(),
							g_SDLPendingCursorData.width,
							g_SDLPendingCursorData.height,
							32,
							g_SDLPendingCursorData.width * sizeof(uint32_t),
							0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);

						if (cursor)
							SDL_FreeCursor(cursor);

						cursor = SDL_CreateColorCursor( cursor_surface, g_SDLPendingCursorData.xhot, g_SDLPendingCursorData.yhot );
						SDL_SetCursor( cursor );
						g_bUpdateSDLCursor = false;
					}
				}
				break;
		}
	}
}

std::optional<pthread_t> sdlwindow_thread;

bool sdlwindow_init( void )
{
	g_SDLInitLock.lock();

	std::thread inputSDLThread( inputSDLThreadRun );
	sdlwindow_thread = inputSDLThread.native_handle();
	inputSDLThread.detach();

	// When this returns SDL_Init should be over
	g_SDLInitLock.lock();

	return g_bSDLInitOK;
}

void sdlwindow_shutdown( void )
{
	if ( sdlwindow_thread )
	{
		pthread_cancel(*sdlwindow_thread);
		sdlwindow_thread = std::nullopt;
	}
}

void sdlwindow_title( std::shared_ptr<std::string> title, std::shared_ptr<std::vector<uint32_t>> icon )
{
	if ( !BIsSDLSession() )
		return;

	{
		std::unique_lock lock(g_SDLWindowTitleLock);

		if ( g_SDLWindowTitle != title )
		{
			g_SDLWindowTitle = title;
			g_bUpdateSDLWindowTitle = true;
		}

		if ( g_SDLWindowIcon != icon )
		{
			g_SDLWindowIcon = icon;
			g_bUpdateSDLWindowIcon = true;
		}

		if ( g_bUpdateSDLWindowTitle || g_bUpdateSDLWindowIcon )
		{
			SDL_Event event;
			event.type = g_unSDLUserEventID + USER_EVENT_TITLE;
			SDL_PushEvent( &event );
		}
	}
}

void sdlwindow_set_selection(std::string contents, int selection)
{
	if (selection == CLIPBOARD)
	{
		SDL_SetClipboardText(contents.c_str());
	}
	else if (selection == PRIMARYSELECTION)
	{
		SDL_SetPrimarySelectionText(contents.c_str());
	}
}

static void set_gamescope_selections()
{
	char *_clipboard = SDL_GetClipboardText();

	char *_primarySelection = SDL_GetPrimarySelectionText();

	gamescope_set_selection(_clipboard, CLIPBOARD);
	gamescope_set_selection(_primarySelection, PRIMARYSELECTION);

	SDL_free(_clipboard);
	SDL_free(_primarySelection);
}

void sdlwindow_visible( bool bVisible )
{
	if ( !BIsSDLSession() )
		return;

	SDL_Event event;
	event.type = g_unSDLUserEventID + USER_EVENT_VISIBLE;
	event.user.code = bVisible ? 1 : 0;
	SDL_PushEvent( &event );
}

void sdlwindow_grab( bool bGrab )
{
	if ( !BIsSDLSession() )
		return;

	if ( g_bForceRelativeMouse )
		return;

	static bool s_bWasGrabbed = false;

	if ( s_bWasGrabbed == bGrab )
		return;

	s_bWasGrabbed = bGrab;

	SDL_Event event;
	event.type = g_unSDLUserEventID + USER_EVENT_GRAB;
	event.user.code = bGrab ? 1 : 0;
	SDL_PushEvent( &event );
}

void sdlwindow_cursor(std::shared_ptr<std::vector<uint32_t>> pixels, uint32_t width, uint32_t height, uint32_t xhot, uint32_t yhot)
{
	if ( !BIsSDLSession() )
		return;

	if ( g_bForceRelativeMouse )
		return;

	{
		std::unique_lock lock( g_SDLCursorLock );
		g_SDLPendingCursorData.width = width;
		g_SDLPendingCursorData.height = height;
		g_SDLPendingCursorData.xhot = xhot;
		g_SDLPendingCursorData.yhot = yhot;
		g_SDLPendingCursorData.data = pixels;
		g_bUpdateSDLCursor = true;
	}

	SDL_Event event;
	event.type = g_unSDLUserEventID + USER_EVENT_CURSOR;
	SDL_PushEvent( &event );
}
