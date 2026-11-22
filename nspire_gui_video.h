/* Menu / file browser UI declarations for NSPIRE_LIBRETRO (legacy gpSP video API). */
#ifndef NSPIRE_GUI_VIDEO_H
#define NSPIRE_GUI_VIDEO_H

void update_screen(void);
void init_video(void);
void video_resolution_large(void);
void video_resolution_small(void);
void clear_screen(u16 color);
u16 *copy_screen(void);
void flip_screen(void);
/* Shows the menus' kit frame (nspire_landscape, 320x240 5:6:5, nspire_ui.c). */
void flip_screen_ui(void);
void video_write_mem_savestate(file_tag_type savestate_file);
void video_read_savestate(file_tag_type savestate_file);


extern u32 frame_speed;

extern s32 affine_reference_x[2];
extern s32 affine_reference_y[2];

typedef void (*tile_render_function)(u32 layer_number, u32 start, u32 end, void *dest_ptr);
typedef void (*bitmap_render_function)(u32 start, u32 end, void *dest_ptr);

typedef struct
{
  tile_render_function normal_render_base;
  tile_render_function normal_render_transparent;
  tile_render_function alpha_render_base;
  tile_render_function alpha_render_transparent;
  tile_render_function color16_render_base;
  tile_render_function color16_render_transparent;
  tile_render_function color32_render_base;
  tile_render_function color32_render_transparent;
} tile_layer_render_struct;

typedef struct
{
  bitmap_render_function normal_render;
} bitmap_layer_render_struct;

typedef enum
{
  unscaled,
  scaled_aspect,
  fullscreen,
  scaled_raw
} video_scale_type;

typedef enum
{
  filter_nearest,
  filter_bilinear
} video_filter_type;

extern u32 screen_scale;
extern u32 current_scale;
extern u32 screen_filter;

void set_gba_resolution(video_scale_type scale);

#endif
