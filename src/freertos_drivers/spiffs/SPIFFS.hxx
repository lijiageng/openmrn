/** @copyright
 * Copyright (c) 2018, Stuart W Baker
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are  permitted provided that the following conditions are met:
 * 
 *  - Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * @file SPIFFS.hxx
 * This file implements the generic SPIFFS driver.
 *
 * @author Stuart W. Baker
 * @date 1 January 2018
 */

#ifndef _FREERTOS_DRIVERS_SPIFFS_SPIFFS_HXX_
#define _FREERTOS_DRIVERS_SPIFFS_SPIFFS_HXX_

#include "Devtab.hxx"

#include <functional>

#include "spiffs.h"

/// Generic SPIFFS base class
class SPIFFS : public FileSystem
{
public:
    /// Mount the file system.
    /// @param mount_point path in the root file system for the mount point.
    ///                   Must not contain any trailing '/' characters, e.g.
    ///                   "/usr", not "/usr/".  Unlike in linux, each mount
    ///                   must be entirely unique point in the system.  For
    ///                   example, mounting of both "/usr" and "/usr/bin" is
    ///                   is not supported.  However, "/usr" and "/usr1" is
    ///                   supported.  This saves on file open logic.
    void mount(const char *mount_point) override
    {
        int result = do_mount();
        if (result != 0)
        {
            // mounting failed, try reformatting
            format();
            result = do_mount();
        }

        if (result == 0)
        {
            name = mount_point;
            if (postFormatHook_ && formatted_)
            {
                postFormatHook_();
            }
            formatted_ = false;
        }
    }

    /// Format the file system, all data will be lost.  The file system must
    /// not be mounted at the time of calling this.
    void format() override
    {
        HASSERT(SPIFFS_mounted(&fs_) == 0);

        // formatting requires at least one mounting, so mount then unmount
        if (do_mount() == 0)
        {
            SPIFFS_unmount(&fs_);
        }

        HASSERT(SPIFFS_format(&fs_) == 0);
        formatted_ = true;
    }

    /// Porovide mutex lock.
    /// @param fs reference to the file system instance
    static void extern_lock(struct spiffs_t *fs)
    {
        static_cast<SPIFFS*>(fs->user_data)->lock_.lock();
    }

    /// Porovide mutex unlock.
    /// @param fs reference to the file system instance
    static void extern_unlock(struct spiffs_t *fs)
    {
        static_cast<SPIFFS*>(fs->user_data)->lock_.unlock();
    }

protected:
    /// Constructor.
    /// @param post_format_hook method to be called after a clean format of
    ///                         the file system.  This allows the user to prime
    ///                         a clean or factory reset file system with an
    ///                         initial set files.
    SPIFFS(size_t physical_address, size_t size_on_disk,
           size_t erase_block_size, size_t logical_block_size,
           size_t logical_page_size, size_t max_num_open_descriptors = 16,
           size_t cache_pages = 8,
           std::function<void()> post_format_hook = nullptr);

    /// Destructor.
    ~SPIFFS()
    {
        if (name)
        {
            SPIFFS_unmount(&fs_);
        }
        delete[] fdSpace_;
        delete[] workBuffer_;
    }

    /// SPIFFS callback to read flash.
    /// @param fs reference to SPIFFS instance
    /// @param addr adddress location to read
    /// @param size size of read in bytes
    /// @param dst destination buffer for read
    static s32_t flash_read(struct spiffs_t *fs, u32_t addr, u32_t size,
                            u8_t *dst)
    {
        return static_cast<SPIFFS*>(fs->user_data)->flash_read(addr, size, dst);
    }

    /// SPIFFS callback to write flash.
    /// @param fs reference to SPIFFS instance
    /// @param addr adddress location to write
    /// @param size size of write in bytes
    /// @param src source buffer for write
    static s32_t flash_write(struct spiffs_t *fs, u32_t addr, u32_t size,
                             u8_t *src)
    {
        return static_cast<SPIFFS*>(fs->user_data)->flash_write(addr, size, src);
    }

    /// SPIFFS callback to erase flash.
    /// @param fs reference to SPIFFS instance
    /// @param addr adddress location to erase
    /// @param size size of erase region in bytes
    static s32_t flash_erase(struct spiffs_t *fs, u32_t addr, u32_t size)
    {
        return static_cast<SPIFFS*>(fs->user_data)->flash_erase(addr, size);
    }

    /// SPIFFS callback to read flash, in context.
    /// @param addr adddress location to read
    /// @param size size of read in bytes
    /// @param dst destination buffer for read
    virtual int32_t flash_read(uint32_t addr, uint32_t size, uint8_t *dst) = 0;

    /// SPIFFS callback to write flash, in context.
    /// @param addr adddress location to write
    /// @param size size of write in bytes
    /// @param src source buffer for write
    virtual int32_t flash_write(uint32_t addr, uint32_t size, uint8_t *src) = 0;

    /// SPIFFS callback to erase flash, in context.
    /// @param addr adddress location to erase
    /// @param size size of erase region in bytes
    virtual int32_t flash_erase(uint32_t addr, uint32_t size) = 0;

    /// configuration parameters for SPIFFS
    spiffs_config config_;

private:
    /// Open directory metadata structure
    struct OpenDir
    {
        spiffs_DIR    dir_; ///< directory object
        struct dirent dirent_; ///< directory entry
    };

    /// Open a file or device.
    /// @param file file reference for this device
    /// @param path file or device name
    /// @param flags open flags
    /// @param mode open mode, ignored in this implementation
    /// @return 0 upon success, -1 upon failure with errno containing the cause
    int open(File *file, const char *path, int flags, int mode) override;

    /// Close a file or device.
    /// @param file file reference for this device
    /// @param fd file descriptor to close
    /// @return 0 upon success, -1 upon failure with errno containing the cause
    int close(File *file) override;

    /// Remove a file.
    /// @param path file name
    /// @return 0 upon successor or negative error number upon error.
    int unlink(const char *path) override;

    /// Read from a file or device.
    /// @param file file reference for this device
    /// @param buf location to place read data
    /// @param count number of bytes to read
    /// @return number of bytes read upon success, -1 upon failure with errno
    ///         containing the cause
    ssize_t read(File *file, void *buf, size_t count) override;

    /// Write to a file or device.
    /// @param file file reference for this device
    /// @param buf location to find write data
    /// @param count number of bytes to write
    /// @return number of bytes written upon success, -1 upon failure with errno
    ///         containing the cause
    ssize_t write(File *file, const void *buf, size_t count) override;

    /// Seek method.
    /// @param f file reference for this device
    /// @param offset offset in bytes from whence directive
    /// @param whence SEEK_SET if to set the file offset to an abosolute position,
    ///               SEEK_CUR if to set the file offset from current position
    ///               SEEK_END if to set the file offset to the end of the file
    /// @return current offest or negative error number upon error.
    off_t lseek(File* f, off_t offset, int whence) override;

    /** Get the status information of a file or device.
     * @param file file reference for this device
     * @param stat structure to fill status info into
     * @return 0 upon successor or negative error number upon error.
     */
    int fstat(File* file, struct stat *stat) override;

    /** Get the status information of a file or device.
     * @param path file or device name
     * @param stat structure to fill status info into
     * @return 0 upon success, -1 upon failure with errno containing the cause
     */
    int stat(const char *path, struct stat *stat) override;

    /** Synchronize (flush) a file to disk.
     * @param file file reference for this device
     * @return 0 upon success, -1 upon failure with errno containing the cause
     */
    int fsync(File *file) override;

    /// Close a directory.
    /// @param file file reference for this device
    /// @return 0 upon success, -1 upon failure with errno containing the cause

    int closedir(File *file) override;

    /// Open a directory.
    /// @param file file reference for this device
    /// @param name directory path
    /// @return pointer to the open directory on success, NULL on error
    File *opendir(File *file, const char *name) override;

    /// Read the next entry in a directory.
    /// @param file file reference for this device
    /// @return pointer to a struct dirent representing the next directectory
    ///        entry
    struct dirent *readdir(File *file) override;

    /// Translate a SPIFFS specific error number to a standard POSIX errno.
    /// @param spiffs_error SPIFFS specific error number
    /// @return standard POSX errno
    int errno_translate(int spiffs_error);

    /// Helper to mount the file system.
    /// @return 0 if successful, else some error code
    int do_mount()
    {
        return SPIFFS_mount(&fs_, &config_, workBuffer_, fdSpace_,
                            fdSpaceSize_, cache_, cacheSize_, nullptr);
    }

    /// callback to be called post a formating operation
    std::function<void()> postFormatHook_;

    /// whole file system lock
    OSMutex lock_;

    /// file system instance metadata
    spiffs fs_;

    /// work buffer for the file system
    uint8_t *workBuffer_;

    /// size in bytes of the fdSpace_
    uint32_t fdSpaceSize_;

    /// file descriptor metadata
    uint8_t *fdSpace_;

    /// size in bytes of cache_
    uint32_t cacheSize_;

    /// memory for cache
    void *cache_;

    /// has the file system been formatted?
    bool formatted_;

    DISALLOW_COPY_AND_ASSIGN(SPIFFS);
};

#endif // _FREERTOS_DRIVERS_SPIFFS_SPIFFS_HXX_
