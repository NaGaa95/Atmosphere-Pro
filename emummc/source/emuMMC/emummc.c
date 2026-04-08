/*
 * Copyright (c) 2019 m4xw <m4x@m4xw.net>
 * Copyright (c) 2019 Atmosphere-NX
 * Copyright (c) 2019 CTCaer
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stdlib.h>

#include "emummc.h"
#include "emummc_ctx.h"
#include "../utils/fatal.h"

static bool storageSDinitialized = false;
static bool storageEMMCinitialized = false;

static bool sdmmc_first_init_sd = false;

static bool file_based_sd_initialized = false;
static bool file_based_emmc_initialized = false;


// hekate sdmmmc vars
sdmmc_t emmc_sdmmc;
sdmmc_storage_t emmc_storage;
sdmmc_t sd_sdmmc;
sdmmc_storage_t sd_storage;

// init vars
bool init_done = false;
bool custom_driver = true;

// FS funcs
_sdmmc_accessor_gc sdmmc_accessor_gc;
_sdmmc_accessor_sd sdmmc_accessor_sd;
_sdmmc_accessor_nand sdmmc_accessor_nand;
_lock_mutex lock_mutex;
_unlock_mutex unlock_mutex;

// FS misc
void *sd_mutex;
void *nand_mutex;
volatile int *active_partition;
volatile Handle *sdmmc_das_handle;

// FatFS
file_based_ctxt f_emu;
file_based_sd_ctxt f_emu_sd;

FATFS sd_fs;
FATFS emmc_fs;

static void _mount_emmc(bool mount){
    static int count = 0;

    DEBUG_LOG_ARGS("mount EMMC mount: %d, count: %d", mount, count);
    if(mount){
        if(count == 0){
            // not mounted yet, mount emmc
            int res = f_mount(&emmc_fs, "sys:", 1);
            if(res != FR_OK){
                DEBUG_LOG_ARGS("EMMC mount failed (%d)\n", res);
                fatal_abort(Fatal_FatfsMount);
            }
        }
        count++;
    }else{
        count--;
        if(count < 0){
            DEBUG_LOG("EMMC unmount before mount\n");
            fatal_abort(Fatal_FatfsMount);
        }
        if(count == 0){
            f_mount(NULL, "sys:", 1);
        }
    }
}

static void _mount_sd(bool mount){
    static int count = 0;

    DEBUG_LOG_ARGS("mount SD mount: %d, count: %d", mount, count);
    if(mount){
        if(count == 0){
            // not mounted yet, mount sd
            int res = f_mount(&sd_fs, "sdmc:", 1);
            if(res != FR_OK){
                DEBUG_LOG_ARGS("SD mount failed (%d)\n", res);
                fatal_abort(Fatal_FatfsMount);
            }
        }
        count++;
    }else{
        count--;
        if(count < 0){
            DEBUG_LOG("SD unmount before mount\n");
            fatal_abort(Fatal_FatfsMount);
        }
        if(count == 0){
            f_mount(NULL, "sdmc:", 1);
        }
    }
}

static int _get_device_from_type(enum EmummcType type)
{
    switch(type)
    {
    case EmummcType_None:
    case EmummcType_Partition_Emmc:
    case EmummcType_File_Emmc:
    // case emuMMC_EMMC_File:
        return FS_SDMMC_EMMC;
    case EmummcType_Partition_Sd:
    case EmummcType_File_Sd:
        return FS_SDMMC_SD;
    default:
        DEBUG_LOG_ARGS("Invalid type. (%d)", type);
        fatal_abort(Fatal_InvalidEnum);
    }
}

static int _get_target_device(int mmc_id)
{
    enum EmummcType type;
    switch(mmc_id)
    {
    case FS_SDMMC_EMMC:
        type = emuMMC_ctx.EMMC_Type;
        break;
    case FS_SDMMC_SD:
        type = emuMMC_ctx.SD_Type;
        break;
    case FS_SDMMC_GC:
        return FS_SDMMC_GC;
    default:
        DEBUG_LOG_ARGS("Get target dev: Inv dev (%d)", mmc_id);
        fatal_abort(Fatal_InvalidEnum);
    }

    return _get_device_from_type(type);
}

static sdmmc_storage_t *_get_storage_for_device(int mmc_id)
{
    switch(mmc_id)
    {
    case FS_SDMMC_EMMC:
        return &emmc_storage;
    case FS_SDMMC_SD:
        return &sd_storage;
    default:
        DEBUG_LOG_ARGS("Get storage: Inv dev (%d)", mmc_id);
        fatal_abort(Fatal_InvalidEnum);
    }
}

static void _emmc_set_partition(int partition){
    if(partition < 0 || partition >= FS_EMMC_PARTITION_INVALID){
        DEBUG_LOG_ARGS("Set part: Inv part (%d)", partition);
        fatal_abort(Fatal_InvalidPartition);
    }
    if(!nx_emmc_set_partition(partition)){
        DEBUG_LOG_ARGS("Set part failed (%d)", partition);
        fatal_abort(Fatal_PartitionSwitchFail);
    }
}

// partition = FS_EMMC_PARTITION_INVALID restores to active partition, if necessary
static void _ensure_partition(int partition){
    static bool should_restore = false;
    if(partition == FS_EMMC_PARTITION_INVALID){
        // Restore partition, if necessary
        if(should_restore){
          _emmc_set_partition(*active_partition);
          should_restore = false;
        }
    }else{
        // If requested partition not already active, change partition
        if(*active_partition != partition){
            _emmc_set_partition(partition);
            should_restore = true;
        }
    }
}

static void _ensure_correct_partition(int target_mmc_id){
    switch (target_mmc_id) {
    case FS_SDMMC_EMMC:
        if(emuMMC_ctx.EMMC_Type == EmummcType_Partition_Emmc ||
           emuMMC_ctx.EMMC_Type == EmummcType_File_Emmc){
            // Switch to GPP if file or partition based emummc
            _ensure_partition(FS_EMMC_PARTITION_GPP);
        }

        // If EMMC_Type == EmummcType_None, we are not redirecting emmc, don't change partition
        // If SD partition or file based, we don't  care
        break;
    case FS_SDMMC_SD:
        if(emuMMC_ctx.SD_Type == EmummcType_Partition_Emmc || emuMMC_ctx.SD_Type == EmummcType_File_Emmc){
            _ensure_partition(FS_EMMC_PARTITION_GPP);
        }
        break;
    default:
        DEBUG_LOG_ARGS("Ensure correct part: Inv dev (%d)", target_mmc_id);
        fatal_abort(Fatal_InvalidEnum);
    }
}

static void _restore_partition(){
    _ensure_partition(FS_EMMC_PARTITION_INVALID);
}

// ICRY
void mutex_lock_handler(int mmc_id)
{
    int sd_target = _get_target_device(FS_SDMMC_SD);
    int emmc_target = _get_target_device(FS_SDMMC_EMMC);

    switch(mmc_id)
    {
    case FS_SDMMC_EMMC:
        if(sd_target == FS_SDMMC_SD && emmc_target == FS_SDMMC_SD)
        {
            if(custom_driver)
            {
                lock_mutex(sd_mutex);
            }
            lock_mutex(nand_mutex);
        }
        else if(sd_target == FS_SDMMC_SD && emmc_target == FS_SDMMC_EMMC)
        {
            if(custom_driver)
            {
                lock_mutex(nand_mutex);
            }
        }
        else if(sd_target == FS_SDMMC_EMMC && emmc_target == FS_SDMMC_SD)
        {
            if(custom_driver)
            {
                lock_mutex(sd_mutex);
            }
            lock_mutex(nand_mutex);
        }
        else if(sd_target == FS_SDMMC_EMMC && emmc_target == FS_SDMMC_EMMC)
        {
            lock_mutex(sd_mutex);
            if(custom_driver)
            {
                lock_mutex(nand_mutex);
            }
        }
        break;
    case FS_SDMMC_SD:
        if(sd_target == FS_SDMMC_SD && emmc_target == FS_SDMMC_SD)
        {
            if(custom_driver)
            {
                lock_mutex(sd_mutex);
            }
            lock_mutex(nand_mutex);
        }
        else if(sd_target == FS_SDMMC_SD && emmc_target == FS_SDMMC_EMMC)
        {
            if(custom_driver)
            {
                lock_mutex(sd_mutex);
            }
        }
        else if(sd_target == FS_SDMMC_EMMC && emmc_target == FS_SDMMC_SD)
        {
            lock_mutex(sd_mutex);
            if(custom_driver)
            {
                lock_mutex(nand_mutex);
            }
        }
        else if(sd_target == FS_SDMMC_EMMC && emmc_target == FS_SDMMC_EMMC)
        {
            lock_mutex(sd_mutex);
            if(custom_driver)
            {
                lock_mutex(nand_mutex);
            }
        }
        break;
    default:
        break;
    }
}

void mutex_unlock_handler(int mmc_id)
{
    int sd_target = _get_target_device(FS_SDMMC_SD);
    int emmc_target = _get_target_device(FS_SDMMC_EMMC);

    switch(mmc_id)
    {
    case FS_SDMMC_EMMC:
        if(sd_target == FS_SDMMC_SD && emmc_target == FS_SDMMC_SD)
        {
            unlock_mutex(nand_mutex);
            if(custom_driver)
            {
                unlock_mutex(sd_mutex);
            }
        }
        else if(sd_target == FS_SDMMC_SD && emmc_target == FS_SDMMC_EMMC)
        {
            if(custom_driver)
            {
                unlock_mutex(nand_mutex);
            }
        }
        else if(sd_target == FS_SDMMC_EMMC && emmc_target == FS_SDMMC_SD)
        {
            unlock_mutex(nand_mutex);
            if(custom_driver)
            {
                unlock_mutex(sd_mutex);
            }
        }
        else if(sd_target == FS_SDMMC_EMMC && emmc_target == FS_SDMMC_EMMC)
        {
            if(custom_driver)
            {
                unlock_mutex(nand_mutex);
            }
            unlock_mutex(sd_mutex);
        }
        break;
    case FS_SDMMC_SD:
        if(sd_target == FS_SDMMC_SD && emmc_target == FS_SDMMC_SD)
        {
            unlock_mutex(nand_mutex);
            if(custom_driver)
            {
                unlock_mutex(sd_mutex);
            }
        }
        else if(sd_target == FS_SDMMC_SD && emmc_target == FS_SDMMC_EMMC)
        {
            if(custom_driver)
            {
                unlock_mutex(sd_mutex);
            }
        }
        else if(sd_target == FS_SDMMC_EMMC && emmc_target == FS_SDMMC_SD)
        {
            if(custom_driver)
            {
                unlock_mutex(nand_mutex);
            }
            unlock_mutex(sd_mutex);
        }
        else if(sd_target == FS_SDMMC_EMMC && emmc_target == FS_SDMMC_EMMC)
        {
            if(custom_driver)
            {
                unlock_mutex(nand_mutex);
            }
            unlock_mutex(sd_mutex);
        }
        break;
    default:
        break;
    }
}

static void _sdmmc_ensure_device_attached(int mmc_id)
{
    // This ensures that the sd device address space handle is always attached,
    // even if FS hasn't attached it
    static bool did_attach = false;
    if (!did_attach)
    {
        // DeviceName_SDMMC1A = 19
        svcAttachDeviceAddressSpace(19, *sdmmc_das_handle);
        did_attach = true;
    }
}

static void _file_based_update_filename(char *outFilename, unsigned int sd_path_len, unsigned int part_idx)
{
    snprintf(outFilename + sd_path_len, 3, "%02d", part_idx);
}

static void _file_based_sd_flush()
{
    if((emuMMC_ctx.SD_Type == EmummcType_File_Emmc || emuMMC_ctx.SD_Type == EmummcType_File_Sd) && file_based_sd_initialized){
        _ensure_correct_partition(FS_SDMMC_SD);
        int i = 0;
        do{
            int res = f_sync(&f_emu_sd.fp[i]);
            if(res != FR_OK){
                DEBUG_LOG_ARGS("fsync failed (err: %d, part: %d)", res, i);
                fatal_abort(Fatal_FatfsSync);
            }
            i++;
        }while(i < f_emu_sd.parts);
        _restore_partition();
    }
}

static void _file_based_sd_initialize(void)
{
    _ensure_correct_partition(FS_SDMMC_SD);

    if(emuMMC_ctx.SD_Type == EmummcType_File_Emmc) {
        _mount_emmc(true);
    }else if(emuMMC_ctx.SD_Type == EmummcType_File_Sd){
        _mount_sd(true);
    }

    char path[sizeof(emuMMC_ctx.SD_storagePath) + 0x20];
    memset(&path, 0, sizeof(path));

    memcpy(path, (void*)emuMMC_ctx.SD_storagePath, sizeof(emuMMC_ctx.SD_storagePath));
    strcat(path, "/SD/");
    int path_len = strlen(path);
    int res;

    _file_based_update_filename(path, path_len, 00);
    res = f_open(&f_emu_sd.fp[0], path, FA_READ | FA_WRITE);
    if(res != FR_OK){
        DEBUG_LOG_ARGS("Open emuSD failed (%d)\n", res);
        fatal_abort(Fatal_FatfsFileOpen);
    }
    if(!f_expand_cltbl(&f_emu_sd.fp[0], EMUSD_FP_CLMT_COUNT, &f_emu_sd.clmt[0][0], f_size(&f_emu_sd.fp[0]))){
        DEBUG_LOG_ARGS("emuSD expand cltbl failed\n"
                       "path: %s\n", path);
        fatal_abort(Fatal_FatfsMemExhaustion);
    }
    f_emu_sd.part_size = (uint64_t)f_size(&f_emu_sd.fp[0]) >> 9;
    f_emu_sd.total_sct = f_emu_sd.part_size;

    for(f_emu_sd.parts = 1; f_emu_sd.parts < EMUSD_FILE_MAX_PARTS; f_emu_sd.parts++){
        _file_based_update_filename(path, path_len, f_emu_sd.parts);

        res = f_open(&f_emu_sd.fp[f_emu_sd.parts], path, FA_READ | FA_WRITE);
        if(res != FR_OK){
            DEBUG_LOG_ARGS("Open emuSD failed (%d)\n"
                           "path: %s\n", res, path);
            // Check if single file.
            if (f_emu_sd.parts == 1)
                f_emu_sd.parts = 0;

            break;
        }

        if(!f_expand_cltbl(&f_emu_sd.fp[f_emu_sd.parts], EMUSD_FP_CLMT_COUNT, &f_emu_sd.clmt[f_emu_sd.parts][0], f_size(&f_emu_sd.fp[f_emu_sd.parts]))){
            DEBUG_LOG_ARGS("emuSD expand cltbl failed\n"
                           "path: %s\n", path);
            fatal_abort(Fatal_FatfsMemExhaustion);
        }

        f_emu_sd.total_sct +=(uint64_t)f_size(&f_emu_sd.fp[f_emu_sd.parts]) >> 9;
    }

    file_based_sd_initialized = true;

    _restore_partition();
}

static void _sdmmc_ensure_initialized_sd(void)
{
    // First Initial init
    if (!sdmmc_first_init_sd)
    {
        sdmmc_initialize_sd();
        sdmmc_first_init_sd = true;
    }
    else
    {
        // The boot sysmodule will eventually kill power to SD.
        // Detect this, and reinitialize when it happens.
        if (!init_done)
        {
            if (sdmmc_get_sd_power_enabled() == 0)
            {
                sdmmc_finalize_sd();
                sdmmc_initialize_sd();
                init_done = true;
            }
        }
    }
}

static void _sdmmc_ensure_initialized_emmc(void)
{
    sdmmc_initialize_emmc();
}

static void _file_based_emmc_finalize(void)
{
    if ((emuMMC_ctx.EMMC_Type == EmummcType_File_Emmc || emuMMC_ctx.EMMC_Type == EmummcType_File_Sd)  && file_based_emmc_initialized)
    {
        _ensure_correct_partition(FS_SDMMC_EMMC);
        // Close all open handles.
        f_close(&f_emu.fp_boot0);
        f_close(&f_emu.fp_boot1);

        {
            int i = 0;
            do {
                f_close(&f_emu.fp_gpp[i]);
                i++;
            } while (i < f_emu.parts);
        }

        // Force unmount FAT volume.
        if (emuMMC_ctx.EMMC_Type == EmummcType_File_Emmc) {
            _mount_emmc(false);
        } else {
            _mount_sd(false);
        }

        file_based_emmc_initialized = false;
        _restore_partition();
    }
}

static void _file_based_sd_finalize(void)
{
    if((emuMMC_ctx.SD_Type == EmummcType_File_Emmc || emuMMC_ctx.SD_Type == EmummcType_File_Sd) && file_based_sd_initialized){
        _ensure_correct_partition(FS_SDMMC_SD);
        int i = 0;
        do {
            f_close(&f_emu_sd.fp[i]);
            i++;
        } while(i < f_emu_sd.parts);

        if(emuMMC_ctx.SD_Type == EmummcType_File_Emmc) {
            _mount_emmc(false);
        }else{
            _mount_sd(false);
        }

        file_based_sd_initialized = false;
        _restore_partition();
    }
}

static void _nand_patrol_ensure_integrity(void)
{
    static bool nand_patrol_checked = false;
    fs_nand_patrol_t nand_patrol;

    if (!nand_patrol_checked)
    {
        if (emuMMC_ctx.EMMC_Type == EmummcType_Partition_Emmc || 
            emuMMC_ctx.EMMC_Type == EmummcType_Partition_Sd   || 
            emuMMC_ctx.EMMC_Type == EmummcType_None)
        {
            sdmmc_storage_t *storage = _get_storage_for_device(_get_device_from_type(emuMMC_ctx.EMMC_Type));

            unsigned int nand_patrol_sector = emuMMC_ctx.EMMC_StoragePartitionOffset + NAND_PATROL_SECTOR;

            if (emuMMC_ctx.EMMC_Type == EmummcType_None) {
                // When eMMC redirection disabled, need to access physical BOOT0
                _ensure_partition(FS_EMMC_PARTITION_BOOT0);
            } else {
                _ensure_partition(FS_EMMC_PARTITION_GPP);
            }
            if (sdmmc_storage_read(storage, nand_patrol_sector, 1, &nand_patrol)){
                // Clear nand patrol if last offset exceeds storage.
                if (nand_patrol.offset > storage->sec_cnt)
                {
                    memset(&nand_patrol, 0, sizeof(fs_nand_patrol_t));
                    sdmmc_storage_write(storage, nand_patrol_sector, 1, &nand_patrol);
                }
            }
            _restore_partition();
            goto out;
        }
        else if ((emuMMC_ctx.EMMC_Type == EmummcType_File_Sd || emuMMC_ctx.EMMC_Type == EmummcType_File_Emmc) && file_based_emmc_initialized)
        {
            FIL *fp = &f_emu.fp_boot0;
            if (f_lseek(fp, NAND_PATROL_OFFSET) != FR_OK)
                goto out;

            if (f_read_fast(fp, &nand_patrol, sizeof(fs_nand_patrol_t)) != FR_OK)
                goto out;

            // Clear nand patrol if last offset exceeds total file based size.
            if (nand_patrol.offset > f_emu.total_sect)
            {
                memset(&nand_patrol, 0, sizeof(fs_nand_patrol_t));

                if (f_lseek(fp, NAND_PATROL_OFFSET) != FR_OK)
                    goto out;

                if (f_write_fast(fp, &nand_patrol, sizeof(fs_nand_patrol_t)) != FR_OK)
                    goto out;

                f_sync(fp);
            }
        }

out:
        nand_patrol_checked = true;
    }
}

static void _sdmmc_ensure_initialized(int mmc_id)
{
    int target_device = _get_target_device(mmc_id);
    if(target_device == FS_SDMMC_SD)
        _sdmmc_ensure_initialized_sd();
    else if(target_device == FS_SDMMC_EMMC){
        _sdmmc_ensure_initialized_emmc();
    }
    // Check if nand patrol offset is inside limits.
    _nand_patrol_ensure_integrity();

    if(mmc_id == FS_SDMMC_SD) {
        // When SD is closed, file based emuSD is deinitialized, but SD may not be deinitialized if still used for eMMC
        // Make sure, file based emuSD is initialized before accessing
        if(!file_based_sd_initialized && (emuMMC_ctx.SD_Type == EmummcType_File_Sd || emuMMC_ctx.SD_Type == EmummcType_File_Emmc)){
            _file_based_sd_initialize();
        }
    }
}

void sdmmc_finalize_sd(void)
{
    if (!sdmmc_storage_end(&sd_storage)) {
        DEBUG_LOG("SD end failed");
        fatal_abort(Fatal_InitSD);
    }

    storageSDinitialized = false;
}

static void _file_based_emmc_initialize(void)
{
    _ensure_correct_partition(FS_SDMMC_EMMC);

    if(emuMMC_ctx.EMMC_Type == EmummcType_File_Emmc) {
        _mount_emmc(true);
    }else if (emuMMC_ctx.EMMC_Type == EmummcType_File_Sd) {
        _mount_sd(true);
    }

    char path[sizeof(emuMMC_ctx.storagePath) + 0x20];
    memset(&path, 0, sizeof(path));

    memcpy(path, (void *)emuMMC_ctx.storagePath, sizeof(emuMMC_ctx.storagePath));
    strcat(path, "/eMMC/");
    int path_len = strlen(path);
    int res;

    // Open BOOT0 physical partition.
    memcpy(path + path_len, "BOOT0", 6);
    res = f_open(&f_emu.fp_boot0, path, FA_READ | FA_WRITE);
    if (res != FR_OK) {
        DEBUG_LOG_ARGS("Open BOOT0 failed (%d)\n"
                       "path: %s\n", res, path);
        fatal_abort(Fatal_FatfsFileOpen);
    }
    if (!f_expand_cltbl(&f_emu.fp_boot0, EMUMMC_FP_CLMT_COUNT, f_emu.clmt_boot0, f_size(&f_emu.fp_boot0))){
        DEBUG_LOG_ARGS("BOOT0 expand cltbl failed\n"
                       "path: %s\n", path);
        fatal_abort(Fatal_FatfsMemExhaustion);
    }

    // Open BOOT1 physical partition.
    memcpy(path + path_len, "BOOT1", 6);
    res = f_open(&f_emu.fp_boot1, path, FA_READ | FA_WRITE);
    if (res != FR_OK){
        DEBUG_LOG_ARGS("Open BOOT1 failed (%d)\n"
                       "path: %s\n", res, path);
        fatal_abort(Fatal_FatfsFileOpen);
    }
    if (!f_expand_cltbl(&f_emu.fp_boot1, EMUMMC_FP_CLMT_COUNT, f_emu.clmt_boot1, f_size(&f_emu.fp_boot1))) {
        DEBUG_LOG_ARGS("BOOT1 expand cltbl failed\n"
                       "path: %s\n", path);
        fatal_abort(Fatal_FatfsMemExhaustion);
    }

    // Open handles for GPP physical partition files.
    _file_based_update_filename(path, path_len, 00);

    res = f_open(&f_emu.fp_gpp[0], path, FA_READ | FA_WRITE);
    if (res != FR_OK){
        DEBUG_LOG_ARGS("Open GPP failed (%d)\n"
                       "path: %s\n", res, path);
        fatal_abort(Fatal_FatfsFileOpen);
    }
    if (!f_expand_cltbl(&f_emu.fp_gpp[0], EMUMMC_FP_CLMT_COUNT, &f_emu.clmt_gpp[0], f_size(&f_emu.fp_gpp[0]))){
        DEBUG_LOG_ARGS("GPP expand cltbl failed\n"
                       "path: %s\n", path);
        fatal_abort(Fatal_FatfsMemExhaustion);
    }

    f_emu.part_size = (uint64_t)f_size(&f_emu.fp_gpp[0]) >> 9;
    f_emu.total_sect = f_emu.part_size;

    // Iterate folder for split parts and stop if next doesn't exist.
    for (f_emu.parts = 1; f_emu.parts < EMUMMC_FILE_MAX_PARTS; f_emu.parts++)
    {
        _file_based_update_filename(path, path_len, f_emu.parts);

        res = f_open(&f_emu.fp_gpp[f_emu.parts], path, FA_READ | FA_WRITE);
        if (res != FR_OK)
        {
            DEBUG_LOG_ARGS("Open GPP failed (%d)\n"
                           "path: %s\n", res, path);
            // Check if single file.
            if (f_emu.parts == 1)
                f_emu.parts = 0;

            break;
        }

        if (!f_expand_cltbl(&f_emu.fp_gpp[f_emu.parts], EMUMMC_FP_CLMT_COUNT,
            &f_emu.clmt_gpp[f_emu.parts * EMUMMC_FP_CLMT_COUNT], f_size(&f_emu.fp_gpp[f_emu.parts])))
        {
            DEBUG_LOG_ARGS("GPP expand cltbl failed\n"
                           "path: %s\n", path);
            fatal_abort(Fatal_FatfsMemExhaustion);
        }

        f_emu.total_sect += (uint64_t)f_size(&f_emu.fp_gpp[f_emu.parts]) >> 9;
    }

    file_based_emmc_initialized = true;

    _restore_partition();
}

bool sdmmc_initialize_sd(void)
{
    if (!storageSDinitialized)
    {
        int retries = 3;
        while (retries)
        {
            if (nx_sd_initialize(false))
            {
                storageSDinitialized = true;

                // Init file based emummc.
                if (emuMMC_ctx.EMMC_Type == EmummcType_File_Sd || emuMMC_ctx.SD_Type == EmummcType_File_Sd)
                {
                    if(emuMMC_ctx.EMMC_Type == EmummcType_File_Sd && !file_based_emmc_initialized){
                        _file_based_emmc_initialize();
                    }
                    if(emuMMC_ctx.SD_Type == EmummcType_File_Sd && !file_based_sd_initialized){
                        _file_based_sd_initialize();
                    }
                }

                break;
            }

            retries--;
        }

        if (!storageSDinitialized) {
            DEBUG_LOG("SD initialize failed\n");
            fatal_abort(Fatal_InitSD);
        }
    }

    return storageSDinitialized;
}

bool sdmmc_initialize_emmc(void)
{
    if (!storageEMMCinitialized)
    {
        if(nx_emmc_initialize(false))
        {
            // if(nx_emmc_set_partition(FS_EMMC_PARTITION_GPP)){
            if(nx_emmc_set_partition(*active_partition)){
                storageEMMCinitialized = true;

                if ((emuMMC_ctx.EMMC_Type == EmummcType_File_Emmc || emuMMC_ctx.SD_Type == EmummcType_File_Emmc)) {
                    if(emuMMC_ctx.EMMC_Type == EmummcType_File_Emmc && !file_based_emmc_initialized){
                        _file_based_emmc_initialize();
                    }
                    if(emuMMC_ctx.SD_Type == EmummcType_File_Emmc && !file_based_sd_initialized){
                        _file_based_sd_initialize();
                    }
                }
            }
        }
    }

    if(!storageEMMCinitialized)
    {
        DEBUG_LOG("eMMC initialize failed\n");
        fatal_abort(Fatal_InitMMC);
    }

    return storageEMMCinitialized;
}

void sdmmc_finalize_emmc(void)
{
    if(sdmmc_storage_end(&emmc_storage))
    {
        DEBUG_LOG("eMMC finalize failed\n");
        fatal_abort(Fatal_InitMMC);
    }
    storageEMMCinitialized = false;
}

sdmmc_accessor_t *sdmmc_accessor_get(int mmc_id)
{
    sdmmc_accessor_t *_this;
    switch (mmc_id)
    {
    case FS_SDMMC_EMMC:
        _this = sdmmc_accessor_nand();
        break;
    case FS_SDMMC_SD:
        _this = sdmmc_accessor_sd();
        break;
    case FS_SDMMC_GC:
        _this = sdmmc_accessor_gc();
        break;
    default:
        DEBUG_LOG_ARGS("Accessor get failed (%d)\n", mmc_id);
        fatal_abort(Fatal_InvalidAccessor);
    }

    return _this;
}

int sdmmc_nand_get_active_partition_index()
{
    switch (*active_partition)
    {
    case FS_EMMC_PARTITION_GPP:
        return 2;
    case FS_EMMC_PARTITION_BOOT1:
        return 1;
    case FS_EMMC_PARTITION_BOOT0:
        return 0;
    }

    DEBUG_LOG_ARGS("Get active part failed (%d)\n", *active_partition);
    fatal_abort(Fatal_InvalidAccessor);
}

static uint64_t emummc_read_write_inner(void *buf, unsigned int sector, unsigned int num_sectors, bool is_write)
{
    if (emuMMC_ctx.EMMC_Type == EmummcType_Partition_Sd   ||
        emuMMC_ctx.EMMC_Type == EmummcType_Partition_Emmc ||
        emuMMC_ctx.EMMC_Type == EmummcType_None)
    {
        // change sector only if we redirect emmc
        if(emuMMC_ctx.EMMC_Type != EmummcType_None){
            // raw partition sector offset: emuMMC_ctx.EMMC_StoragePartitionOffset.
            sector += emuMMC_ctx.EMMC_StoragePartitionOffset;
            // Set physical partition offset
            sector += (sdmmc_nand_get_active_partition_index() * BOOT_PARTITION_SIZE);
        }

        sdmmc_storage_t *storage = _get_storage_for_device(_get_device_from_type(emuMMC_ctx.EMMC_Type));

        if (__builtin_expect(sector + num_sectors > storage->sec_cnt, 0))
            return 0; // Out of bounds. Can only happen with Nand Patrol if resized.

        _ensure_correct_partition(FS_SDMMC_EMMC);

        uint64_t ret;
        if (!is_write)
            ret = sdmmc_storage_read(storage, sector, num_sectors, buf);
        else
            ret = sdmmc_storage_write(storage, sector, num_sectors, buf);

        _restore_partition();

        return ret;
    }else if(emuMMC_ctx.EMMC_Type == EmummcType_File_Sd || emuMMC_ctx.EMMC_Type == EmummcType_File_Emmc){
        // File based emummc.
        uint64_t ret;

        _ensure_correct_partition(FS_SDMMC_EMMC);

        FIL *fp = NULL;
        switch (*active_partition)
        {
        case FS_EMMC_PARTITION_GPP:
            if (f_emu.parts)
            {
                if (__builtin_expect(sector + num_sectors > f_emu.total_sect, 0)) {
                    ret = 0; // Out of bounds. Can only happen with Nand Patrol if resized.
                    goto out;
                }

                fp = &f_emu.fp_gpp[sector / f_emu.part_size];
                sector = sector % f_emu.part_size;

                // Special handling for reads/writes which cross file-boundaries.
                if (__builtin_expect(sector + num_sectors > f_emu.part_size, 0))
                {
                    unsigned int remaining = num_sectors;
                    while (remaining > 0) {
                        const unsigned int cur_sectors = MIN(remaining, f_emu.part_size - sector);

                        if (f_lseek(fp, (uint64_t)sector << 9) != FR_OK){
                            ret = 0; // Out of bounds.
                            goto out;
                        }

                        if (!is_write)
                        {
                            if (f_read_fast(fp, buf, (uint64_t)cur_sectors << 9) != FR_OK) {
                                ret = 0;
                                goto out;
                            }
                        }
                        else
                        {
                            if (f_write_fast(fp, buf, (uint64_t)cur_sectors << 9) != FR_OK) {
                                ret = 0;
                                goto out;
                            }
                        }

                        buf = (char *)buf + ((uint64_t)cur_sectors << 9);
                        remaining -= cur_sectors;
                        sector = 0;
                        ++fp;
                    }

                    ret = 1;
                    goto out;
                }
            } else {
                fp = &f_emu.fp_gpp[0];
            }
            break;

        case FS_EMMC_PARTITION_BOOT1:
            fp = &f_emu.fp_boot1;
            break;

        case FS_EMMC_PARTITION_BOOT0:
            fp = &f_emu.fp_boot0;
            break;
        }


        if (f_lseek(fp, (uint64_t)sector << 9) != FR_OK) {
            ret = 0; // Out of bounds. Can only happen with Nand Patrol if resized.
            goto out;
        }

        if (!is_write)
            ret = !f_read_fast(fp, buf, (uint64_t)num_sectors << 9);
        else
            ret = !f_write_fast(fp, buf, (uint64_t)num_sectors << 9);

        out:
        _restore_partition();
        return ret;
    } else {
        DEBUG_LOG_ARGS("Invalid eMMC type (%d)\n", emuMMC_ctx.EMMC_Type);
        fatal_abort(Fatal_InvalidEnum);
    }
}

static uint64_t emummc_read_write_sd_inner(void *buf, unsigned int sector, unsigned int num_sectors, bool is_write)
{
    if(emuMMC_ctx.SD_Type == EmummcType_Partition_Sd || emuMMC_ctx.SD_Type == EmummcType_Partition_Emmc){
        sector += emuMMC_ctx.SD_StoragePartitionOffset;
        sdmmc_storage_t *storage = _get_storage_for_device(_get_device_from_type(emuMMC_ctx.SD_Type));

        // Out of bounds access check, shouldn't ever happen on SD access
        if (__builtin_expect(sector + num_sectors > storage->sec_cnt, 0)){
            DEBUG_LOG_ARGS("OOB SD access sct: 0x%x,\n"
                           "              cnt: 0x%x,\n"
                           "              sz:  0x%x\n", 
                           sector, num_sectors, storage->sec_cnt);
            fatal_abort(Fatal_OOB);
        }

        _ensure_correct_partition(FS_SDMMC_SD);

        uint64_t ret;
        if(!is_write){
            ret = sdmmc_storage_read(storage, sector, num_sectors, buf);
        }else{
            ret = sdmmc_storage_write(storage, sector, num_sectors, buf);
        }

        _restore_partition();

        return ret;
    }else if (emuMMC_ctx.SD_Type == EmummcType_File_Emmc || emuMMC_ctx.SD_Type == EmummcType_File_Sd){
        // File based emummc.
        uint64_t ret;

        _ensure_correct_partition(FS_SDMMC_SD);

        FIL *fp = NULL;
        if (f_emu_sd.parts)
        {
            if (__builtin_expect(sector + num_sectors > f_emu_sd.total_sct, 0)) {
                ret = 0; // Out of bounds. Can only happen with Nand Patrol if resized.
                goto out;
            }

            fp = &f_emu_sd.fp[sector / f_emu_sd.part_size];
            sector = sector % f_emu_sd.part_size;

            // Special handling for reads/writes which cross file-boundaries.
            if (__builtin_expect(sector + num_sectors > f_emu_sd.part_size, 0))
            {
                unsigned int remaining = num_sectors;
                while (remaining > 0) {
                    const unsigned int cur_sectors = MIN(remaining, f_emu_sd.part_size - sector);

                    if (f_lseek(fp, (uint64_t)sector << 9) != FR_OK){
                        ret = 0; // Out of bounds.
                        goto out;
                    }

                    if (!is_write)
                    {
                        if (f_read_fast(fp, buf, (uint64_t)cur_sectors << 9) != FR_OK) {
                            ret = 0;
                            goto out;
                        }
                    }
                    else
                    {
                        if (f_write_fast(fp, buf, (uint64_t)cur_sectors << 9) != FR_OK) {
                            ret = 0;
                            goto out;
                        }
                    }

                    buf = (char *)buf + ((uint64_t)cur_sectors << 9);
                    remaining -= cur_sectors;
                    sector = 0;
                    ++fp;
                }

                ret = 1;
                goto out;
            }
        } else {
            fp = &f_emu_sd.fp[0];
        }


        if (f_lseek(fp, (uint64_t)sector << 9) != FR_OK) {
            ret = 0; // Out of bounds. Can only happen with Nand Patrol if resized.
            goto out;
        }

        if (!is_write)
            ret = !f_read_fast(fp, buf, (uint64_t)num_sectors << 9);
        else
            ret = !f_write_fast(fp, buf, (uint64_t)num_sectors << 9);

        out:
        _restore_partition();
        return ret;
    }else{
        // File based sd redirection not supported currently
        DEBUG_LOG_ARGS("Invalid emuSD type (%d)\n", emuMMC_ctx.SD_Type);
        fatal_abort(Fatal_InvalidEnum);
    }
}

// Controller open wrapper
uint64_t sdmmc_wrapper_controller_open(int mmc_id)
{
    uint64_t result;
    sdmmc_accessor_t *_this;
    _this = sdmmc_accessor_get(mmc_id);

    
    if (_this != NULL)
    {
        if (mmc_id == FS_SDMMC_SD)
        {
            // Lock eMMC while SD is initialized by FS
            if(custom_driver)
            {
                lock_mutex(sd_mutex);
            }
            lock_mutex(nand_mutex);

            DEBUG_LOG("Controller open SD\n");
            result = _this->vtab->sdmmc_accessor_controller_open(_this);

            unlock_mutex(nand_mutex);
            if(custom_driver)
            {
                unlock_mutex(sd_mutex);
            }
        } else if (mmc_id == FS_SDMMC_EMMC) {
            DEBUG_LOG("Controller open EMMC\n");
            result = _this->vtab->sdmmc_accessor_controller_open(_this);
        } else {
            DEBUG_LOG("Controller open GC\n");
            result = _this->vtab->sdmmc_accessor_controller_open(_this);
        }

        return result;
    }

    DEBUG_LOG("Controller open fail (was null)\n");
    fatal_abort(Fatal_OpenAccessor);
}

// Controller close wrapper
uint64_t sdmmc_wrapper_controller_close(int mmc_id)
{
    sdmmc_accessor_t *_this;
    _this = sdmmc_accessor_get(mmc_id);

    if (_this != NULL)
    {
        if (mmc_id == FS_SDMMC_SD)
        {

            if(custom_driver)
            {
                lock_mutex(sd_mutex);
            }
            lock_mutex(nand_mutex);

            // Deinitializing/initializing file based emuSD every time SD is opened/closed
            // takes a long time. Instead, keep the files open and flush when SD is closed.
            // Will be finalized when eMMC is finalized
            _file_based_sd_flush();

            unlock_mutex(nand_mutex);
            if(custom_driver)
            {
                unlock_mutex(sd_mutex);
            }

            DEBUG_LOG("Controller Close SD\n");
            if(_get_target_device(FS_SDMMC_EMMC) != FS_SDMMC_SD){
                // eMMC not redirected to SD, can close SD
                uint64_t ret =_this->vtab->sdmmc_accessor_controller_close(_this);
                // sdmmc_storage_end(&sd_storage);
                storageSDinitialized = false;
                sdmmc_first_init_sd = false;
                return ret;
            } else {
                // eMMC redirected to SD, can't close SD yet
                DEBUG_LOG("Still in use!\n");
                return 0;
            }
        }

        if (mmc_id == FS_SDMMC_EMMC)
        {
            // Close file handles and unmount
            DEBUG_LOG("Controller Close eMMC\n");
            _file_based_emmc_finalize();
            _file_based_sd_finalize();

            if(_get_target_device(FS_SDMMC_EMMC) == FS_SDMMC_SD)
            {
                DEBUG_LOG("also close SD\n");
                // When eMMC redirected to SD, also close SD
                // Close SD
                sdmmc_accessor_get(FS_SDMMC_SD)->vtab->sdmmc_accessor_controller_close(sdmmc_accessor_get(FS_SDMMC_SD));
                // sdmmc_storage_end(&sd_storage);
                storageSDinitialized = false;
                sdmmc_first_init_sd = false;
            }

            // Close eMMC
            return _this->vtab->sdmmc_accessor_controller_close(_this);
        }

        return _this->vtab->sdmmc_accessor_controller_close(_this);
    }

    DEBUG_LOG("Controller close fail (was null)\n");
    fatal_abort(Fatal_CloseAccessor);
}

// FS read wrapper.
uint64_t sdmmc_wrapper_read(void *buf, uint64_t bufSize, int mmc_id, unsigned int sector, unsigned int num_sectors)
{
    sdmmc_accessor_t *_this;
    uint64_t read_res;

    _this = sdmmc_accessor_get(mmc_id);

    if (_this != NULL)
    {
        if (mmc_id == FS_SDMMC_EMMC || mmc_id == FS_SDMMC_SD)
        {
            mutex_lock_handler(mmc_id);
            // Assign FS accessor to the SDMMC driver
            _current_accessor = _this;
            // Make sure we're attached to the device address space.
            _sdmmc_ensure_device_attached(mmc_id);
            // Make sure we're still initialized if boot killed sd card power.
            _sdmmc_ensure_initialized(mmc_id);
        }

        if (mmc_id == FS_SDMMC_EMMC)
        {
            // eMMC read
            // Call hekates driver.
            uint64_t res = emummc_read_write_inner(buf, sector, num_sectors, false) ? 0 : FS_READ_WRITE_ERROR;
            mutex_unlock_handler(mmc_id);
            return res;
        }

        if (mmc_id == FS_SDMMC_SD)
        {
            // SD read

            // TODO: Don't swap to fs driver for now
            // static bool first_sd_read = true;
            // if (first_sd_read)
            // {
            //     first_sd_read = false;

            //     if (emuMMC_ctx.EMMC_Type == EmummcType_Partition_Sd && false)
            //     // if (emuMMC_ctx.EMMC_Type == emuMMC_SD_Raw)
            //     {
            //         // Because some SD cards have issues with emuMMC's driver
            //         // we currently swap to FS's driver after first SD read
            //         // for raw based emuMMC
            //         custom_driver = false;
            //         // FS will handle sd mutex w/o custom driver from here on
            //         unlock_mutex(sd_mutex);
            //     }
            // }

            // Call hekate's driver.
            uint64_t res = emummc_read_write_sd_inner(buf, sector, num_sectors, false) ? 0 : FS_READ_WRITE_ERROR;
            mutex_unlock_handler(mmc_id);
            return res;
        }

        read_res = _this->vtab->read_write(_this, sector, num_sectors, buf, bufSize, 1);
        return read_res;
    }

    DEBUG_LOG("Read failed (was null)\n");
    fatal_abort(Fatal_ReadNoAccessor);
}

// FS write wrapper.
uint64_t sdmmc_wrapper_write(int mmc_id, unsigned int sector, unsigned int num_sectors, void *buf, uint64_t bufSize)
{
    sdmmc_accessor_t *_this;
    uint64_t write_res;

    _this = sdmmc_accessor_get(mmc_id);

    if (_this != NULL)
    {
        if (mmc_id == FS_SDMMC_EMMC)
        {
            // eMMC write
            mutex_lock_handler(mmc_id);
            _current_accessor = _this;

            // Call hekates driver.
            uint64_t res = emummc_read_write_inner(buf, sector, num_sectors, true) ? 0 : FS_READ_WRITE_ERROR;
            mutex_unlock_handler(mmc_id);
            return res;
        }

        if (mmc_id == FS_SDMMC_SD)
        {
            // SD write
            mutex_lock_handler(mmc_id);
            _current_accessor = _this;

            // Call hekates driver.
            uint64_t res = emummc_read_write_sd_inner(buf, sector, num_sectors, true) ? 0 : FS_READ_WRITE_ERROR;


            mutex_unlock_handler(mmc_id);
            return res;
        }


        write_res = _this->vtab->read_write(_this, sector, num_sectors, buf, bufSize, 0);
        return write_res;
    }

    DEBUG_LOG("Write failed (was null)\n");
    fatal_abort(Fatal_WriteNoAccessor);
}
