//-*-C-*-
// RSXGL - Graphics library for the PS3 GPU.
//
// Copyright (c) 2011 Alexander Betts (alex.betts@gmail.com)
//
// rsxgl_config.h - Compile-time configuration settings.

#ifndef rsxgl_config_H
#define rsxgl_config_H

#define RSXGL_CONFIG_default_gcm_buffer_size (1024 * 1024 * 4)
#define RSXGL_CONFIG_default_command_buffer_length (0x80000)

#define RSXGL_CONFIG_vertex_migrate_buffer_size (4 * 1024 * 1024)
#define RSXGL_CONFIG_texture_migrate_buffer_size (32 * 1024 * 1024)

#define RSXGL_CONFIG_samples_host_ip ""
#define RSXGL_CONFIG_samples_host_port 0

#define RSXGL_CONFIG_RSX_compatibility 0

#endif
