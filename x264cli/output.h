#pragma once

typedef void *hnd_t;

typedef struct
{
    int use_dts_compress;
} cli_output_opt_t;

typedef struct
{
	int (*configure_x264_param) ( x264_param_t *param );
    int (*open_file)( char *psz_filename, hnd_t *p_handle, cli_output_opt_t *opt );
    int (*set_param)( hnd_t handle, x264_param_t *p_param );
    int (*write_headers)( hnd_t handle, x264_nal_t *p_nal );
    int (*write_frame)( hnd_t handle, uint8_t *p_nal, int i_size, x264_picture_t *p_picture );
    int (*close_file)( hnd_t handle, int64_t largest_pts, int64_t second_largest_pts );
} cli_output_t;

extern const cli_output_t mkv_output;
