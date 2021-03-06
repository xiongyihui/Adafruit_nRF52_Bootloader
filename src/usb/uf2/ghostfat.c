#include "compile_date.h"

#include "uf2.h"
#include "flash_nrf5x.h"
#include <string.h>

#include "bootloader_settings.h"
#include "bootloader.h"

//--------------------------------------------------------------------+
//
//--------------------------------------------------------------------+

typedef struct {
    uint8_t JumpInstruction[3];
    uint8_t OEMInfo[8];
    uint16_t SectorSize;
    uint8_t SectorsPerCluster;
    uint16_t ReservedSectors;
    uint8_t FATCopies;
    uint16_t RootDirectoryEntries;
    uint16_t TotalSectors16;
    uint8_t MediaDescriptor;
    uint16_t SectorsPerFAT;
    uint16_t SectorsPerTrack;
    uint16_t Heads;
    uint32_t HiddenSectors;
    uint32_t TotalSectors32;
    uint8_t PhysicalDriveNum;
    uint8_t Reserved;
    uint8_t ExtendedBootSig;
    uint32_t VolumeSerialNumber;
    uint8_t VolumeLabel[11];
    uint8_t FilesystemIdentifier[8];
} __attribute__((packed)) FAT_BootBlock;

typedef struct {
    char name[8];
    char ext[3];
    uint8_t attrs;
    uint8_t reserved;
    uint8_t createTimeFine;
    uint16_t createTime;
    uint16_t createDate;
    uint16_t lastAccessDate;
    uint16_t highStartCluster;
    uint16_t updateTime;
    uint16_t updateDate;
    uint16_t startCluster;
    uint32_t size;
} __attribute__((packed)) DirEntry;
STATIC_ASSERT(sizeof(DirEntry) == 32);

struct TextFile {
  char const name[11];
  char const *content;
};


//--------------------------------------------------------------------+
//
//--------------------------------------------------------------------+

#define NUM_FAT_BLOCKS CFG_UF2_NUM_BLOCKS

#define STR0(x) #x
#define STR(x) STR0(x)

const char infoUf2File[] = //
    "UF2 Bootloader " UF2_VERSION "\r\n"
    "Model: " UF2_PRODUCT_NAME "\r\n"
    "Board-ID: " UF2_BOARD_ID "\r\n"
    "Date: " __DATE__ "\r\n";

const char indexFile[] = //
    "<!doctype html>\n"
    "<html>"
    "<body>"
    "<script>\n"
    "location.replace(\"" UF2_INDEX_URL "\");\n"
    "</script>"
    "</body>"
    "</html>\n";

// WARNING -- code presumes only one NULL .content for .UF2 file
//            and requires it be the last element of the array
static struct TextFile const info[] = {
    {.name = "INFO_UF2TXT", .content = infoUf2File},
    {.name = "INDEX   HTM", .content = indexFile},
    {.name = "CURRENT UF2"},
};

// WARNING -- code presumes each non-UF2 file content fits in single sector
//            Cannot programmatically statically assert .content length
//            for each element above.
STATIC_ASSERT(ARRAY_SIZE(indexFile) < 512);


#define NUM_FILES          (ARRAY_SIZE(info))
#define NUM_DIRENTRIES     (NUM_FILES + 1) // Code adds volume label as first root directory entry

#define UF2_SIZE           (uf2current_flash_sz * 2)
#define UF2_SECTORS        (UF2_SIZE / 512)
#define UF2_FIRST_SECTOR   (NUM_FILES + 1) // WARNING -- code presumes each non-UF2 file content fits in single sector
#define UF2_LAST_SECTOR    (UF2_FIRST_SECTOR + UF2_SECTORS - 1)

#define RESERVED_SECTORS   1
#define ROOT_DIR_SECTORS   4
#define SECTORS_PER_FAT    ((NUM_FAT_BLOCKS * 2 + 511) / 512)

#define START_FAT0         RESERVED_SECTORS
#define START_FAT1         (START_FAT0 + SECTORS_PER_FAT)
#define START_ROOTDIR      (START_FAT1 + SECTORS_PER_FAT)
#define START_CLUSTERS     (START_ROOTDIR + ROOT_DIR_SECTORS)

// all directory entries must fit in a single sector
// because otherwise current code overflows buffer
#define DIRENTRIES_PER_SECTOR (512/sizeof(DirEntry))

STATIC_ASSERT(NUM_DIRENTRIES < DIRENTRIES_PER_SECTOR * ROOT_DIR_SECTORS);


static FAT_BootBlock const BootBlock = {
    .JumpInstruction      = {0xeb, 0x3c, 0x90},
    .OEMInfo              = "UF2 UF2 ",
    .SectorSize           = 512,
    .SectorsPerCluster    = 1,
    .ReservedSectors      = RESERVED_SECTORS,
    .FATCopies            = 2,
    .RootDirectoryEntries = (ROOT_DIR_SECTORS * DIRENTRIES_PER_SECTOR),
    .TotalSectors16       = NUM_FAT_BLOCKS - 2,
    .MediaDescriptor      = 0xF8,
    .SectorsPerFAT        = SECTORS_PER_FAT,
    .SectorsPerTrack      = 1,
    .Heads                = 1,
    .PhysicalDriveNum     = 0x80, // to match MediaDescriptor of 0xF8
    .ExtendedBootSig      = 0x29,
    .VolumeSerialNumber   = 0x00420042,
    .VolumeLabel          = UF2_VOLUME_LABEL,
    .FilesystemIdentifier = "FAT16   ",
};

//--------------------------------------------------------------------+
//
//--------------------------------------------------------------------+
static inline bool is_uf2_block (UF2_Block const *bl)
{
  return (bl->magicStart0 == UF2_MAGIC_START0) && (bl->magicStart1 == UF2_MAGIC_START1) &&
         (bl->magicEnd == UF2_MAGIC_END) &&
         (bl->flags & UF2_FLAG_FAMILYID) && !(bl->flags & UF2_FLAG_NOFLASH) &&
         (bl->payloadSize == 256) && !(bl->targetAddr & 0xff);
}

static inline bool in_app_space (uint32_t addr)
{
  return USER_FLASH_START <= addr && addr < USER_FLASH_END;
}

static inline bool in_uicr_space(uint32_t addr)
{
  return addr == 0x10001000;
}

static inline bool in_bootloader_space (uint32_t addr)
{
  return USER_FLASH_END <= addr && addr < CFG_UF2_FLASH_SIZE;
}

static inline bool in_softdevice_space (uint32_t addr)
{
  return addr < USER_FLASH_START;
}

//--------------------------------------------------------------------+
//
//--------------------------------------------------------------------+
static uint32_t uf2current_flash_sz = 0;


// get current.uf2 flash size in bytes, round up to 256 bytes
static uint32_t current_flash_size(void)
{
  uint32_t flash_sz = 0;

  // return 1 block of 256 bytes
  if ( !bootloader_app_is_valid(DFU_BANK_0_REGION_START) )
  {
    flash_sz = 256;
  }else
  {
    bootloader_settings_t const * boot_setting;
    bootloader_util_settings_get(&boot_setting);

    flash_sz = boot_setting->bank_0_size;

    // Copy size must be multiple of 256 bytes
    // else we will got an issue copying current.uf2
    if (flash_sz & 0xff)
    {
      flash_sz = (flash_sz & ~0xff) + 256;
    }

    // if bank0 size is not valid, happens when flashed with jlink
    // use maximum application size
    if ( (flash_sz == 0) || (flash_sz == 0xFFFFFFFFUL) )
    {
      flash_sz = (USER_FLASH_END-USER_FLASH_START);
    }
  }

  return flash_sz;
}

void uf2_init(void)
{
  uf2current_flash_sz = current_flash_size();
}

/*------------------------------------------------------------------*/
/* Read CURRENT.UF2
 *------------------------------------------------------------------*/
void padded_memcpy (char *dst, char const *src, int len)
{
  for ( int i = 0; i < len; ++i )
  {
    if ( *src )
      *dst = *src++;
    else
      *dst = ' ';
    dst++;
  }
}

void read_block(uint32_t block_no, uint8_t *data) {
    memset(data, 0, 512);
    uint32_t sectionIdx = block_no;

    if (block_no == 0) { // Requested boot block
        memcpy(data, &BootBlock, sizeof(BootBlock));
        data[510] = 0x55;
        data[511] = 0xaa;
        // logval("data[0]", data[0]);
    } else if (block_no < START_ROOTDIR) {  // Requested FAT table sector
        sectionIdx -= START_FAT0;
        // logval("sidx", sectionIdx);
        if (sectionIdx >= SECTORS_PER_FAT)
            sectionIdx -= SECTORS_PER_FAT; // second FAT is same as the first...
        if (sectionIdx == 0) {
            data[0] = 0xf8; // first FAT entry must match BPB MediaDescriptor
            // WARNING -- code presumes only one NULL .content for .UF2 file
            //            and all non-NULL .content fit in one sector
            //            and requires it be the last element of the array
            for (uint32_t i = 1; i < NUM_FILES * 2 + 4; ++i) {
                data[i] = 0xff;
            }
        }
        for (uint32_t i = 0; i < 256; ++i) { // Generate the FAT chain for the firmware "file"
            uint32_t v = sectionIdx * 256 + i;
            if (UF2_FIRST_SECTOR <= v && v <= UF2_LAST_SECTOR)
                ((uint16_t *)(void *)data)[i] = v == UF2_LAST_SECTOR ? 0xffff : v + 1;
        }
    } else if (block_no < START_CLUSTERS) { // Requested root directory sector

        sectionIdx -= START_ROOTDIR;

        DirEntry *d = (void *)data;
        int remainingEntries = DIRENTRIES_PER_SECTOR;
        if (sectionIdx == 0) { // volume label first
            // volume label is first directory entry
            padded_memcpy(d->name, (char const *) BootBlock.VolumeLabel, 11);
            d->attrs = 0x28;
            d++;
            remainingEntries--;
        }

        for (uint32_t i = DIRENTRIES_PER_SECTOR * sectionIdx;
             remainingEntries > 0 && i < NUM_FILES;
             i++, d++) {

            // WARNING -- code presumes all but last file take exactly one sector
            uint16_t startCluster = i + 2;

            struct TextFile const * inf = &info[i];
            padded_memcpy(d->name, inf->name, 11);
            d->createTimeFine   = __SECONDS_INT__ % 2 * 100;
            d->createTime       = __DOSTIME__;
            d->createDate       = __DOSDATE__;
            d->lastAccessDate   = __DOSDATE__;
            d->highStartCluster = startCluster >> 8;
            // DIR_WrtTime and DIR_WrtDate must be supported
            d->updateTime       = __DOSTIME__;
            d->updateDate       = __DOSDATE__;
            d->startCluster     = startCluster & 0xFF;
            // WARNING -- code presumes only one NULL .content for .UF2 file
            //            and requires it be the last element of the array
            d->size = inf->content ? strlen(inf->content) : UF2_SIZE;
        }

    } else {
        sectionIdx -= START_CLUSTERS;
        if (sectionIdx < NUM_FILES - 1) {
            memcpy(data, info[sectionIdx].content, strlen(info[sectionIdx].content));
        } else { // generate the UF2 file data on-the-fly
            sectionIdx -= NUM_FILES - 1;
            uint32_t addr = USER_FLASH_START + sectionIdx * 256;
            if (addr < CFG_UF2_FLASH_SIZE) {
                UF2_Block *bl = (void *)data;
                bl->magicStart0 = UF2_MAGIC_START0;
                bl->magicStart1 = UF2_MAGIC_START1;
                bl->magicEnd = UF2_MAGIC_END;
                bl->blockNo = sectionIdx;
                bl->numBlocks = uf2current_flash_sz / 256;
                bl->targetAddr = addr;
                bl->payloadSize = 256;
                bl->flags = UF2_FLAG_FAMILYID;
                bl->familyID = CFG_UF2_FAMILY_ID;
                memcpy(bl->data, (void *)addr, bl->payloadSize);
            }
        }
    }
}

/*------------------------------------------------------------------*/
/* Write UF2
 *------------------------------------------------------------------*/

/**
 * Write an uf2 block wrapped by 512 sector. Writing behavior is different when upgrading:
 * - Application
 *    - current App is erased and flash with new firmware with same starting address
 *
 * - SoftDevice + Bootloader
 *    - Current App is erased, contents of SD + bootloader is written to App starting address
 *    - Trigger the SD + Bootloader migration
 *
 * @return number of bytes processed, only 3 following values
 *  -1 : if not an uf2 block
 * 512 : write is successful
 *   0 : is busy with flashing, tinyusb stack will call write_block again with the same parameters later on
 */
int write_block (uint32_t block_no, uint8_t *data, WriteState *state)
{
  UF2_Block *bl = (void*) data;

  if ( !is_uf2_block(bl) ) return -1;

  PRINTF("Addr = 0x%08lX, payloadSize = %ld, Family = 0x%08lX\n", bl->targetAddr, bl->payloadSize, bl->familyID);
#if 0
  uint32_t wr_addr;

  switch ( bl->familyID )
  {
    case CFG_UF2_FAMILY_ID:
      // Upgrade application
      // Directly write over application -- > Target address is the written address
      if ( !in_app_space(bl->targetAddr) ) return -1;

      wr_addr = bl->targetAddr;
    break;

    case CFG_UF2_BOOTLOADER_ID:
      /* Upgrading SoftDevice + Bootloader combo
       *
       * To prevent corruption/disconnection while transferring we don't directly write over
       * SoftDevice and Bootloader. Instead SD + Bootloader is written to Application region.
       * Once everything is received and verified.They are written to their respective address
       *
       * Note: to simplify the upgrade process, we ASSUME
       *  - Bootloader size is fixed and is the same as the old bootloader
       *  - New SoftDevice can has different size than the old one
       *
       *                         -------------         -------------         -------------
       *                        |             |       |             |     + |     New     |
       *                        | Bootloader  |       | Bootloader  |    +  | Bootloader  |
       * BOOTLOADER_ADDR_START--|-------------|       |-------------|   +   |-------------|
       *                        |             |       |    New      |  +    |             |
       *                        |             |       | Bootloader  | +     |             |
       *                        |             |       |  ++++++++   |       |             |
       *                        | Application | --->  |             |       |  Invalid    |
       *                        |             |       |             |       | Application |
       *                        |             |       |  ++++++++   |       |             |
       *                        |             |       |    New      |       |             |
       *                        |             |       | SoftDevice  | +     |-------------|
       *      USER_FLASH_START--|-------------|       |-------------|  +    |             |
       *                        |             |       |             |   +   |             |
       *                        |             |       |             |    +  |     New     |
       *                        | SoftDevice  |       | SoftDevice  |     + | SoftDevice  |
       *                         -------------         -------------         -------------
       */

      if ( in_uicr_space(bl->targetAddr) )
      {
        /* UCIR contains bootloader & MBR address as follow:
         * - 0x10001014: for bootloader address
         * - 0x10001018: for MBR
         * Both values are fixed if bootloader size is not change
         */

        // Nothing to do since bootloader size is fixed
        return 512;
      }
      else if ( in_softdevice_space(bl->targetAddr) )
      {
        // Right above the old SoftDevice
        wr_addr = USER_FLASH_START + bl->targetAddr;
      }
      else if ( in_bootloader_space(bl->targetAddr) )
      {
        // Right below the old Bootloader
        //wr_addr = USER_FLASH_START + bl->targetAddr;
      }
      else
      {

        return -1;
      }
    break;

    // unknown family ID
    default: return -1;
  }

  (void) wr_addr;
#endif

  // only accept block with same family id
  if ( (bl->familyID != CFG_UF2_FAMILY_ID) ) return -1;

  if ( (bl->targetAddr < USER_FLASH_START) || (bl->targetAddr + bl->payloadSize > USER_FLASH_END) )
  {

  }
  else
  {
    //PRINTF("Write block at %x", bl->targetAddr);
    flash_nrf5x_write(bl->targetAddr, bl->data, bl->payloadSize, true);
  }

  if ( state && bl->numBlocks )
  {
    // Update state num blocks
    if ( state->numBlocks != bl->numBlocks )
    {
      if ( bl->numBlocks >= MAX_BLOCKS || state->numBlocks )
        state->numBlocks = 0xffffffff;
      else
        state->numBlocks = bl->numBlocks;
    }

    if ( bl->blockNo < MAX_BLOCKS )
    {
      uint8_t const mask = 1 << (bl->blockNo % 8);
      uint32_t const pos = bl->blockNo / 8;

      // only increase written number with new write (possibly prevent overwriting from OS)
      if ( !(state->writtenMask[pos] & mask) )
      {
        state->writtenMask[pos] |= mask;
        state->numWritten++;
      }

      // flush last blocks
      if ( state->numWritten >= state->numBlocks )
      {
        flash_nrf5x_flush(true);
      }
    }

    //PRINTF("wr %d=%d (of %d)", state->numWritten, bl->blockNo, bl->numBlocks);
  }

  return 512;
}
