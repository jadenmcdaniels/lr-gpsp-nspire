#include "common.h"
#include "input.h"
#include "nspire.h"
#include "nspire_audio.h"
#include "nspire_bt_glue.h"
#include "nspire_keys.h"
#include "nspire_ui_screens.h"
#include <string.h>

/* Called before the framebuffer is returned to the OS. There is no
 * unsafe 'force exit': the old callback still needs its code and buffers. */
void nspire_audio_draining(void)
{
  static const char *const no_actions[] = { NULL };
  struct ui_manager_view v;
  memset(&v, 0, sizeof(v));
  v.title = "Closing USB audio"; v.name = "sound-closing";
  v.empty = "Waiting for the adapter";
  v.pic_empty = ""; v.percent = -1; v.actions = no_actions;
  v.status = "Taking long? Unplug the audio adapter.";   /* 38 characters: the band holds 45 */
  ui_manager_draw(&v);
}
