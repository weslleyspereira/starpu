/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2008-2023  Université de Bordeaux, CNRS (LaBRI UMR 5800), Inria
 * Copyright (C) 2015       Mathieu Lirzin
 *
 * StarPU is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * StarPU is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU Lesser General Public License in COPYING.LGPL for more details.
 */

#ifndef __DRIVER_CUDA_H__
#define __DRIVER_CUDA_H__

/** @file */

#include <common/config.h>

void _starpu_cuda_preinit(void);

#ifdef STARPU_USE_CUDA
#include <cuda.h>
#include <cuda_runtime_api.h>
#ifdef STARPU_HAVE_LIBNVIDIA_ML
#include <nvml.h>
#endif
#endif

#include <starpu.h>
#include <core/workers.h>
#include <datawizard/node_ops.h>

#pragma GCC visibility push(hidden)

extern struct _starpu_driver_ops _starpu_driver_cuda_ops;
extern struct _starpu_node_ops _starpu_driver_cuda_node_ops;

extern int _starpu_nworker_per_cuda;

void _starpu_cuda_init(void);
unsigned _starpu_get_cuda_device_count(void);
#ifdef STARPU_HAVE_HWLOC
struct _starpu_machine_topology;
hwloc_obj_t _starpu_cuda_get_hwloc_obj(hwloc_topology_t topology, int devid);
#endif
extern int _starpu_cuda_bus_ids[STARPU_MAXCUDADEVS+STARPU_MAXNUMANODES][STARPU_MAXCUDADEVS+STARPU_MAXNUMANODES];

#if defined(STARPU_USE_CUDA) || defined(STARPU_SIMGRID)
void _starpu_cuda_discover_devices (struct _starpu_machine_config *);
void _starpu_init_cuda_config(struct _starpu_machine_topology *topology, struct _starpu_machine_config *);
void _starpu_cuda_init_worker_binding(struct _starpu_machine_config *config, int no_mp_config, struct _starpu_worker *workerarg);
void _starpu_cuda_init_worker_memory(struct _starpu_machine_config *config, int no_mp_config, struct _starpu_worker *workerarg);
void _starpu_init_cuda(void);
void _starpu_init_cublas_v2_func(void);
void _starpu_shutdown_cublas_v2_func(void);
void _starpu_cublas_v2_init(void);
void _starpu_cublas_v2_shutdown(void);
void *_starpu_cuda_worker(void *);
#ifdef STARPU_HAVE_LIBNVIDIA_ML
nvmlDevice_t _starpu_cuda_get_nvmldev(struct cudaDeviceProp *props);
#endif
#else
#  define _starpu_cuda_discover_devices(config) ((void) config)
#endif

#ifdef STARPU_USE_CUDA
#ifdef STARPU_USE_CUDA_MAP
uintptr_t _starpu_cuda_map_ram(uintptr_t src_ptr, size_t src_offset, unsigned src_node, unsigned dst_node, size_t size, int *ret);
int _starpu_cuda_unmap_ram(uintptr_t src_ptr, size_t src_offset, unsigned src_node, uintptr_t dst_ptr, unsigned dst_node, size_t size);
int _starpu_cuda_update_map(uintptr_t src, size_t src_offset, unsigned src_node, uintptr_t dst, size_t dst_offset, unsigned dst_node, size_t size);
#endif
#endif

unsigned _starpu_cuda_test_request_completion(struct _starpu_async_channel *async_channel);
void _starpu_cuda_wait_request_completion(struct _starpu_async_channel *async_channel);

int _starpu_cuda_copy_interface_from_cpu_to_cuda(starpu_data_handle_t handle, void *src_interface, unsigned src_node, void *dst_interface, unsigned dst_node, struct _starpu_data_request *req);
int _starpu_cuda_copy_interface_from_cuda_to_cuda(starpu_data_handle_t handle, void *src_interface, unsigned src_node, void *dst_interface, unsigned dst_node, struct _starpu_data_request *req);
int _starpu_cuda_copy_interface_from_cuda_to_cpu(starpu_data_handle_t handle, void *src_interface, unsigned src_node, void *dst_interface, unsigned dst_node, struct _starpu_data_request *req);

int _starpu_cuda_copy_data_from_cuda_to_cuda(uintptr_t src, size_t src_offset, unsigned src_node, uintptr_t dst, size_t dst_offset, unsigned dst_node, size_t size, struct _starpu_async_channel *async_channel);
int _starpu_cuda_copy_data_from_cuda_to_cpu(uintptr_t src, size_t src_offset, unsigned src_node, uintptr_t dst, size_t dst_offset, unsigned dst_node, size_t size, struct _starpu_async_channel *async_channel);
int _starpu_cuda_copy_data_from_cpu_to_cuda(uintptr_t src, size_t src_offset, unsigned src_node, uintptr_t dst, size_t dst_offset, unsigned dst_node, size_t size, struct _starpu_async_channel *async_channel);

int _starpu_cuda_copy2d_data_from_cuda_to_cuda(uintptr_t src, size_t src_offset, unsigned src_node, uintptr_t dst, size_t dst_offset, unsigned dst_node, size_t blocksize, size_t numblocks, size_t ld_src, size_t ld_dst, struct _starpu_async_channel *async_channel);
int _starpu_cuda_copy2d_data_from_cuda_to_cpu(uintptr_t src, size_t src_offset, unsigned src_node, uintptr_t dst, size_t dst_offset, unsigned dst_node, size_t blocksize, size_t numblocks, size_t ld_src, size_t ld_dst, struct _starpu_async_channel *async_channel);
int _starpu_cuda_copy2d_data_from_cpu_to_cuda(uintptr_t src, size_t src_offset, unsigned src_node, uintptr_t dst, size_t dst_offset, unsigned dst_node, size_t blocksize, size_t numblocks, size_t ld_src, size_t ld_dst, struct _starpu_async_channel *async_channel);

int _starpu_cuda_is_direct_access_supported(unsigned node, unsigned handling_node);
uintptr_t _starpu_cuda_malloc_on_node(unsigned dst_node, size_t size, int flags);
void _starpu_cuda_free_on_node(unsigned dst_node, uintptr_t addr, size_t size, int flags);

#pragma GCC visibility pop

#endif //  __DRIVER_CUDA_H__

