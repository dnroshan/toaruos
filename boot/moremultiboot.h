
static mboot_mod_t modules_mboot[sizeof(modules)/sizeof(*modules)] = {
	{0,0,0,1}
};

static struct multiboot multiboot_header = {
	/* flags;             */ MULTIBOOT_FLAG_CMDLINE | MULTIBOOT_FLAG_MODS | MULTIBOOT_FLAG_MEM | MULTIBOOT_FLAG_MMAP | MULTIBOOT_FLAG_LOADER,
	/* mem_lower;         */ 0x100000,
	/* mem_upper;         */ 0x640000,
	/* boot_device;       */ 0,
	/* cmdline;           */ 0,
	/* mods_count;        */ sizeof(modules)/sizeof(*modules),
	/* mods_addr;         */ 0,
	/* num;               */ 0,
	/* size;              */ 0,
	/* addr;              */ 0,
	/* shndx;             */ 0,
	/* mmap_length;       */ 0,
	/* mmap_addr;         */ 0,
	/* drives_length;     */ 0,
	/* drives_addr;       */ 0,
	/* config_table;      */ 0,
	/* boot_loader_name;  */ 0,
	/* apm_table;         */ 0,
	/* vbe_control_info;  */ 0,
	/* vbe_mode_info;     */ 0,
	/* vbe_mode;          */ 0,
	/* vbe_interface_seg; */ 0,
	/* vbe_interface_off; */ 0,
	/* vbe_interface_len; */ 0,
};

static long ramdisk_off = 1;
static long ramdisk_len = 1;

int _eax = 1;
int _ebx = 1;
int _xmain = 1;

struct mmap_entry {
	uint64_t base;
	uint64_t len;
	uint32_t type;
	uint32_t reserved;
};

extern unsigned short mmap_ent;
extern unsigned short lower_mem;

char * final_offset = NULL;

extern char do_the_nasty[];

static int strlen(char * s) {
	int out = 0;
	while (*s) {
		s++;
		out++;
	}
	return out;
}

#ifdef EFI_PLATFORM
static EFI_GUID efi_graphics_output_protocol_guid =
  {0x9042a9de,0x23dc,0x4a38,  {0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}};
#define KERNEL_LOAD_START 0x1600000
#else
#define KERNEL_LOAD_START 0x300000
#endif

static void move_kernel(void) {
	clear();
	print("Relocating kernel...\n");

	Elf32_Header * header = (Elf32_Header *)KERNEL_LOAD_START;

	if (header->e_ident[0] != ELFMAG0 ||
	    header->e_ident[1] != ELFMAG1 ||
	    header->e_ident[2] != ELFMAG2 ||
	    header->e_ident[3] != ELFMAG3) {
		print_("Kernel is invalid?\n");
		while (1) {};
	}

	uintptr_t entry = (uintptr_t)header->e_entry;

	for (uintptr_t x = 0; x < (uint32_t)header->e_phentsize * header->e_phnum; x += header->e_phentsize) {
		Elf32_Phdr * phdr = (Elf32_Phdr *)((uint8_t*)KERNEL_LOAD_START + header->e_phoff + x);
		if (phdr->p_type == PT_LOAD) {
			//read_fs(file, phdr->p_offset, phdr->p_filesz, (uint8_t *)phdr->p_vaddr);
			print("Loading a Phdr... ");
			print_hex(phdr->p_vaddr);
			print(" ");
			print_hex(phdr->p_offset);
			print(" ");
			print_hex(phdr->p_filesz);
			print("\n");
#ifdef EFI_PLATFORM
			EFI_PHYSICAL_ADDRESS addr = phdr->p_vaddr;
			EFI_STATUS status = uefi_call_wrapper(ST->BootServices->AllocatePages, 3, AllocateAddress, EfiLoaderData, phdr->p_memsz / 4096 + 1, &addr);
			if (EFI_ERROR(status)) {
				print_("Failed to allocate memory to load kernel.\n");
				while (1) {};
			}
#endif
			memcpy((uint8_t*)(uintptr_t)phdr->p_vaddr, (uint8_t*)KERNEL_LOAD_START + phdr->p_offset, phdr->p_filesz);
			long r = phdr->p_filesz;
			while (r < phdr->p_memsz) {
				*(char *)(phdr->p_vaddr + r) = 0;
				r++;
			}
		}
	}

	print("Setting up memory map...\n");
#ifdef EFI_PLATFORM
	mboot_memmap_t * mmap = (void*)KERNEL_LOAD_START;
	memset((void*)KERNEL_LOAD_START, 0x00, 1024);
	multiboot_header.mmap_addr = (uint32_t)(uintptr_t)mmap;

	EFI_STATUS e;
	UINTN mapSize = 0, mapKey, descriptorSize;
	UINT32 descriptorVersion;
	e = uefi_call_wrapper(ST->BootServices->GetMemoryMap, 5, &mapSize, NULL, &mapKey, &descriptorSize, NULL);

	print_("Memory map size is "); print_hex_(mapSize); print_("\n");

	EFI_MEMORY_DESCRIPTOR * efi_memory = (void*)(final_offset);
	final_offset += mapSize;
	while ((uintptr_t)final_offset & 0x3ff) final_offset++;

	e = uefi_call_wrapper(ST->BootServices->GetMemoryMap, 5, &mapSize, efi_memory, &mapKey, &descriptorSize, NULL);

	if (EFI_ERROR(e)) {
		print_("EFI error.\n");
		while (1) {};
	}

	uint64_t upper_mem = 0;
	int descriptors = mapSize / descriptorSize;
	for (int i = 0; i < descriptors; ++i) {
		EFI_MEMORY_DESCRIPTOR * d = efi_memory;

		mmap->size = sizeof(uint64_t) * 2 + sizeof(uint32_t);
		mmap->base_addr = d->PhysicalStart;
		mmap->length = d->NumberOfPages * 4096;
		switch (d->Type) {
			case EfiConventionalMemory:
			case EfiLoaderCode:
			case EfiLoaderData:
			case EfiBootServicesCode:
			case EfiBootServicesData:
			case EfiRuntimeServicesCode:
			case EfiRuntimeServicesData:
			case EfiACPIReclaimMemory:
				mmap->type = 1;
				break;
			case EfiReservedMemoryType:
			case EfiUnusableMemory:
			case EfiMemoryMappedIO:
			case EfiMemoryMappedIOPortSpace:
			case EfiPalCode:
			case EfiACPIMemoryNVS:
			default:
				mmap->type = 2;
				break;
		}
		if (mmap->type == 1 && mmap->base_addr >= 0x100000) {
			upper_mem += mmap->length;
		}
		mmap = (mboot_memmap_t *) ((uintptr_t)mmap + mmap->size + sizeof(uint32_t));
		efi_memory = (EFI_MEMORY_DESCRIPTOR *)((char *)efi_memory + descriptorSize);
	}

	multiboot_header.mmap_length = (uintptr_t)mmap - multiboot_header.mmap_addr;

	multiboot_header.mem_lower = 1024;
	multiboot_header.mem_upper = upper_mem / 1024;

	/* Set up framebuffer */
	if (_efi_do_mode_set) {
		UINTN count;
		EFI_HANDLE * handles;
		EFI_GRAPHICS_OUTPUT_PROTOCOL * gfx;
		EFI_STATUS status;

		status = uefi_call_wrapper(ST->BootServices->LocateHandleBuffer,
				5, ByProtocol, &efi_graphics_output_protocol_guid, NULL, &count, &handles);
		if (EFI_ERROR(status)) {
			print_("Error getting graphics device handle.\n");
			while (1) {};
		}

		status = uefi_call_wrapper(ST->BootServices->HandleProtocol,
				3, handles[0], &efi_graphics_output_protocol_guid, (void **)&gfx);
		if (EFI_ERROR(status)) {
			print_("Error getting graphics device.\n");
			while (1) {};
		}

#if 0
		print_("Attempting to set a sane mode (32bit color, etc.)\n");
		print_("There are 0x"); print_hex_(gfx->Mode->MaxMode); print_(" modes available.\n");
		print_("This is mode 0x"); print_hex_(gfx->Mode->Mode); print_(".\n");
#endif

		int biggest = gfx->Mode->Mode;
		int big_width = 0;
		int big_height = 0;

		clear_();

		for (int i = 0; i < gfx->Mode->MaxMode; ++i) {
			EFI_STATUS status;
			UINTN size;
			EFI_GRAPHICS_OUTPUT_MODE_INFORMATION * info;

			status = uefi_call_wrapper(gfx->QueryMode,
					4, gfx, i, &size, &info);

			if (EFI_ERROR(status)) {
				print_("Error getting gfx mode 0x"); print_hex_(i); print_("\n");
			} else {
				print_("Mode "); print_int_(i); print_(" "); print_int_(info->HorizontalResolution);
				print_("x"); print_int_(info->VerticalResolution); print_(" ");
				print_int_(info->PixelFormat); print_("\n");
				if (_efi_do_mode_set == 1) {
					if (info->PixelFormat == 1 && info->HorizontalResolution >= big_width) {
						biggest = i;
						big_width = info->HorizontalResolution;
					}
				} else if (_efi_do_mode_set == 2) {
					if (info->PixelFormat == 1 && info->HorizontalResolution == 1024 &&
						info->VerticalResolution == 768) {
						biggest = i;
						break;
					}
				} else if (_efi_do_mode_set == 3) {
					if (info->PixelFormat == 1 && info->HorizontalResolution == 1920 &&
						info->VerticalResolution == 1080) {
						biggest = i;
						break;
					}
				} else if (_efi_do_mode_set == 4) {
					while (1) {
						print_("y/n? ");
						int resp = read_scancode();
						if (resp == 'y') {
							print_("y\n");
							biggest = i;
							goto done_video;
						} else if (resp == 'n') {
							print_("n\n");
							break;
						}
						print_("?\n");
					}
				}
			}
		}

done_video:
		print_("Selected video mode was "); print_int_(biggest); print_("\n");
		uefi_call_wrapper(gfx->SetMode, 2, gfx, biggest);

		uint32_t high = gfx->Mode->FrameBufferBase >> 32;
		uint32_t low  = gfx->Mode->FrameBufferBase & 0xFFFFFFFF;

		print_("Framebuffer address is 0x"); print_hex_(high); print_hex_(low); print_("\n");

		if (high) {
			clear_();
			print_("Framebuffer is outside of 32-bit memory range.\n");
			print_("EFI mode setting is not available - and graphics may not work in general.\n");
			while (1) {};
		}

		multiboot_header.flags |= (1 << 12); /* Enable framebuffer flag */
		multiboot_header.framebuffer_addr = low;
		multiboot_header.framebuffer_width  = gfx->Mode->Info->HorizontalResolution;
		multiboot_header.framebuffer_height = gfx->Mode->Info->VerticalResolution;
		multiboot_header.framebuffer_pitch = gfx->Mode->Info->PixelsPerScanLine * 4;

		print_("Mode information passed to multiboot:\n");
		print_("  Address: 0x"); print_hex_(multiboot_header.framebuffer_addr); print_("\n");
		print_("  Width:   "); print_int_(multiboot_header.framebuffer_width); print_("\n");
		print_("  Height:  "); print_int_(multiboot_header.framebuffer_height); print_("\n");
		print_("\n");

	}

	memcpy(final_offset, cmdline, strlen(cmdline)+1);
	multiboot_header.cmdline = (uintptr_t)final_offset;
	final_offset += strlen(cmdline)+1;
	memcpy(final_offset, VERSION_TEXT, strlen(VERSION_TEXT)+1);
	multiboot_header.boot_loader_name = (uintptr_t)final_offset;
	final_offset += strlen(VERSION_TEXT)+1;
	while ((uintptr_t)final_offset & 0x3ff) final_offset++;

	multiboot_header.mods_addr = (uintptr_t)final_offset;
	memcpy(final_offset, &modules_mboot, sizeof(modules_mboot));
	final_offset += sizeof(modules_mboot);
	while ((uintptr_t)final_offset & 0x3ff) final_offset++;

	memcpy(final_offset, &multiboot_header, sizeof(multiboot_header));
	_ebx = (uintptr_t)final_offset;


	print("Jumping to main, good luck.\n");
#else
	print_hex(mmap_ent);
	print("\n");
	memset((void*)KERNEL_LOAD_START, 0x00, 1024);
	mboot_memmap_t * mmap = (void*)KERNEL_LOAD_START;
	multiboot_header.mmap_addr = (uintptr_t)mmap;
	multiboot_header.mods_addr = (uintptr_t)&modules_mboot;
	multiboot_header.boot_loader_name = (uintptr_t)VERSION_TEXT;

	struct mmap_entry * e820 = (void*)0x5000;

	uint64_t upper_mem = 0;
	for (int i = 0; i < mmap_ent; ++i) {
		print("entry "); print_hex(i);
		print(" "); print_hex((uint32_t)(e820[i].base >> 32ULL)); print_hex((uint32_t)e820[i].base);
		print(" "); print_hex((uint32_t)(e820[i].len >> 32ULL)); print_hex((uint32_t)e820[i].len);
		print(" "); print_hex(e820[i].type); print("\n");

		mmap->size = sizeof(uint64_t) * 2 + sizeof(uintptr_t);
		mmap->base_addr = e820[i].base;
		mmap->length = e820[i].len;
		mmap->type = e820[i].type;
		if (mmap->type == 1 && mmap->base_addr >= 0x100000) {
			upper_mem += mmap->length;
		}
		mmap = (mboot_memmap_t *) ((uintptr_t)mmap + mmap->size + sizeof(uintptr_t));
	}
	multiboot_header.mmap_length = (uintptr_t)mmap - multiboot_header.mmap_addr;

	print("lower "); print_hex(lower_mem); print("KB\n");
	multiboot_header.mem_lower = 1024;
	print("upper ");
	print_hex(upper_mem >> 32);
	print_hex(upper_mem);
	print("\n");
	
	multiboot_header.mem_upper = upper_mem / 1024;

	_ebx = (unsigned int)&multiboot_header;
#endif

	_eax = MULTIBOOT_EAX_MAGIC;
	_xmain = entry;

#ifdef EFI_PLATFORM
	//print_("\nExiting boot services and jumping to ");
	//print_hex_(_xmain);
	//print_(" with mboot_mag=");
	//print_hex_(_eax);
	//print_(" and mboot_ptr=");
	//print_hex_(_ebx);
	//print_("...\n");

#if defined(__x86_64__)
	//print_("&_xmain = "); print_hex_(((uintptr_t)&_xmain) >> 32); print_hex_((uint32_t)(uintptr_t)&_xmain); print_("\n");
#endif

	if (1) {
		EFI_STATUS e;
		UINTN mapSize = 0, mapKey, descriptorSize;
		UINT32 descriptorVersion;
		uefi_call_wrapper(ST->BootServices->GetMemoryMap, 5, &mapSize, NULL, &mapKey, &descriptorSize, NULL);
		e = uefi_call_wrapper(ST->BootServices->ExitBootServices, 2, ImageHandleIn, mapKey);

		if (e != EFI_SUCCESS) { 
			print_("Exit services failed. \n");
			print_hex_(e);
			while (1) {};
		}
	}
#endif

#if defined(__x86_64__)
	uint64_t foobar = ((uint32_t)(uintptr_t)&do_the_nasty) | (0x10L << 32L);

	uint32_t * foo = (uint32_t *)0x7c00;

	foo[0] = _eax;
	foo[1] = _ebx;
	foo[2] = _xmain;

	__asm__ __volatile__ (
			"push %0\n"
			"retf\n"
			 : : "g"(foobar));
#else
	uint32_t foo[3];
	foo[0] = _eax;
	foo[1] = _ebx;
	foo[2] = _xmain;
	__asm__ __volatile__ (
		"mov %%cr0,%%eax\n"
		/* Disable paging */
		"and $0x7FFeFFFF, %%eax\n"
		"mov %%eax,%%cr0\n"
		/* Ensure PAE is not enabled */
		"mov %%cr4,%%eax\n"
		"and $0xffffffdf, %%eax\n"
		"mov %%eax,%%cr4\n"
		"mov %1,%%eax \n"
		"mov %2,%%ebx \n"
		"jmp *%0" : : "g"(foo[2]), "g"(foo[0]), "g"(foo[1]) : "eax", "ebx"
		);
#endif
}

#if defined(__x86_64__)
	__asm__ (
		"do_the_nasty:\n"
		"cli\n"
		//"mov 0x08, %ax\n"
		//"mov %ax, %ds\n"
		//"mov %ax, %es\n"
		//"mov %ax, %fs\n"
		//"mov %ax, %gs\n"
		//"mov %ax, %ss\n"
		".code32\n"
		"mov %cr0, %eax\n"
		"and $0x7FFeFFFF, %eax\n"
		"mov %eax, %cr0\n"
		// Paging is disabled
		"mov $0xc0000080, %ecx\n"
		"rdmsr\n"
		"and $0xfffffeff, %eax\n"
		"wrmsr\n"
		"mov $0x640, %eax\n"
		"mov %eax, %cr4\n"
		"mov 0x7c00, %eax\n"
		"mov 0x7c04, %ebx\n"
		"mov 0x7c08, %ecx\n"
		"jmp *%ecx\n"
		"target: jmp target\n"
		".code64\n"
		);
#endif

__attribute__((always_inline))
static inline uint8_t read_byte(void) {
	return *inputPtr++;
}
__attribute__((always_inline))
static inline void _write(unsigned int sym) {
	*outputPtr++ = sym;
}

#ifndef EFI_PLATFORM
static void do_it(struct ata_device * _device) {
	device = _device;
	if (device->atapi_sector_size != 2048) {
		print_hex_(device->atapi_sector_size);
		print_("\n - bad sector size\n");
		return;
	}
	for (int i = 0x10; i < 0x15; ++i) {
		ata_device_read_sector_atapi(device, i, (uint8_t *)root);
		switch (root->type) {
			case 1:
				root_sector = i;
				goto done;
			case 0xFF:
				print_("Bad read\n");
				return;
		}
	}
	print_("Early return?\n");
	return;
done:
	restore_root();

	if (navigate(kernel_path)) {
		print("Found kernel.\n");
		print_hex(dir_entry->extent_start_LSB); print(" ");
		print_hex(dir_entry->extent_length_LSB); print("\n");
		long offset = 0;
		for (int i = dir_entry->extent_start_LSB; i < dir_entry->extent_start_LSB + dir_entry->extent_length_LSB / 2048 + 1; ++i, offset += 2048) {

			ata_device_read_sector_atapi(device, i, (uint8_t *)KERNEL_LOAD_START + offset);
		}
		while (offset % 4096) offset++;
		restore_root();
		if (navigate(module_dir)) {
			memcpy(mod_dir, dir_entry, sizeof(iso_9660_directory_entry_t));
			print("Scanning modules...\n");
			char ** c = modules;
			int j = 0;
			while (*c) {
				print("load "); print(*c); print("\n");
				if (!navigate(*c)) {
					print("Failed to locate module! [");
					print(*c);
					multiboot_header.mods_count--;
					print("]\n");
				} else {
					modules_mboot[j].mod_start = KERNEL_LOAD_START + offset;
					modules_mboot[j].mod_end = KERNEL_LOAD_START + offset + dir_entry->extent_length_LSB;
					for (int i = dir_entry->extent_start_LSB; i < dir_entry->extent_start_LSB + dir_entry->extent_length_LSB / 2048 + 1; ++i, offset += 2048) {
						ata_device_read_sector_atapi(device, i, (uint8_t *)KERNEL_LOAD_START + offset);
					}
					while (offset % 4096) offset++;
					j++;
				}
				c++;
				restore_mod();
			}
			print("Done.\n");
			restore_root();
			if (navigate(ramdisk_path)) {
				//clear_();
				ramdisk_off = KERNEL_LOAD_START + offset;

				print_("Loading ramdisk");

				int i = dir_entry->extent_start_LSB;
				int sectors = dir_entry->extent_length_LSB / 2048 + 1;
				size_t off = 0;
#if 0
#define RAMDISK_OFFSET 0x5000000
#else
#define RAMDISK_OFFSET ramdisk_off
#endif
#define SECTORS 512
				while (sectors >= SECTORS) {
					print_(".");
					ata_device_read_sectors_atapi(device, i, (uint8_t *)RAMDISK_OFFSET + off, SECTORS);

					sectors -= SECTORS;
					off += 2048 * SECTORS;
					i += SECTORS;
				}
				if (sectors > 0) {
					print_("!");
					ata_device_read_sectors_atapi(device, i, (uint8_t *)RAMDISK_OFFSET + off, sectors);
					off += 2048 * sectors;
				}

				#if 0
				inputPtr = (uint8_t*)RAMDISK_OFFSET;
				outputPtr = (uint8_t*)ramdisk_off;
				print_("\nDecompressing ramdisk... ");
				if (gzip_decompress()) {
					print_("Failed?\n");
				}
				print_("Done!\n");
				ramdisk_len = (uint32_t)outputPtr - ramdisk_off;
				#else
				ramdisk_len = dir_entry->extent_length_LSB;
				outputPtr = ramdisk_off + ramdisk_len;
				#endif

				modules_mboot[multiboot_header.mods_count-1].mod_start = ramdisk_off;
				modules_mboot[multiboot_header.mods_count-1].mod_end = ramdisk_off + ramdisk_len;

				final_offset = (outputPtr) + ((uintptr_t)outputPtr) % 4096;

				set_attr(0x07);
				print("Done.\n");
				move_kernel();
			}
		} else {
			print("No mod directory?\n");
		}
	} else {
		print("boo\n");
	}

	return;
}
#endif

struct fw_cfg_file {
	uint32_t size;
	uint16_t select;
	uint16_t reserved;
	char name[56];
};

static int boot_mode = 0;

void swap_bytes(void * in, int count) {
	char * bytes = in;
	if (count == 4) {
		uint32_t * t = in;
		*t = (bytes[0] << 24) | (bytes[1] << 12) | (bytes[2] << 8) | bytes[3];
	} else if (count == 2) {
		uint16_t * t = in;
		*t = (bytes[0] << 8) | bytes[1];
	}
}

void show_menu(void) {

#if 1
	/* Try to detect qemu headless boot */
	outports(0x510, 0x0000);
	if (inportb(0x511) == 'Q' &&
		inportb(0x511) == 'E' &&
		inportb(0x511) == 'M' &&
		inportb(0x511) == 'U') {
		uint32_t count = 0;
		uint8_t * bytes = (uint8_t *)&count;
		outports(0x510,0x0019);
		for (int i = 0; i < 4; ++i) {
			bytes[i] = inportb(0x511);
		}
		swap_bytes(&count, 4);
#if 0
		print_("there are ");
		print_hex_(count);
		print_(" entries\n");
#endif

		unsigned int bootmode_size = 0;
		int bootmode_index = -1;
		for (unsigned int i = 0; i < count; ++i) {
			struct fw_cfg_file file;
			uint8_t * tmp = (uint8_t *)&file;
			for (int j = 0; j < sizeof(struct fw_cfg_file); ++j) {
				tmp[j] = inportb(0x511);
			}
			if (!strcmp(file.name,"opt/org.toaruos.bootmode")) {
				swap_bytes(&file.size, 4);
				swap_bytes(&file.select, 2);
				bootmode_size = file.size;
				bootmode_index = file.select;
			}
#if 0
			print_("selector "); print_hex_(file.select); print_(" is "); print_hex_(file.size); print_(" bytes\n");
			print_("and its name is: "); print_(file.name); print_("\n");
#endif
		}

		if (bootmode_index != -1) {
			outports(0x510, bootmode_index);
			char tmp[33] = {0};
			for (int i = 0; i < 32 && i < bootmode_size; ++i) {
				tmp[i] = inportb(0x511);
			}
			for (int i = 0; i < BASE_SEL+1; ++i) {
				if (!strcmp(tmp,boot_mode_names[i].key)) {
					boot_mode = boot_mode_names[i].index;
					return;
				}
			}
			print_("fw_cfg boot mode not recognized: ");
			print_(tmp);
			print_("\n");
		}
	}
#endif


	/* Determine number of options */
	sel_max = 0;
	while (boot_options[sel_max].value) {
		sel_max++;
	}
	sel_max += BASE_SEL + 1;

#ifndef EFI_PLATFORM
	outportb(0x3D4, 14);
	outportb(0x3D5, 0xFF);
	outportb(0x3D4, 15);
	outportb(0x3D5, 0xFF);

	inportb(0x3DA);
	outportb(0x3C0, 0x30);
	char b = inportb(0x3C1);
	b &= ~8;
	outportb(0x3c0, b);
#endif

	clear_();

	do {
		move_cursor(0,0);
		set_attr(0x1f);
		print_banner(VERSION_TEXT);
		set_attr(0x07);
		print_("\n");

		for (int i = 0; i < BASE_SEL+1; ++i) {
			set_attr(sel == i ? 0x70 : 0x07);
			print_(" ");
			char tmp[] = {'0' + (i + 1), '.', ' ', '\0'};
			print_(tmp);
			print_(boot_mode_names[i].title);
			print_("\n");
		}

		// put a gap
		set_attr(0x07);
		print_("\n");

		for (int i = 0; i < sel_max - BASE_SEL - 1; ++i) {
			toggle(BASE_SEL + 1 + i, *boot_options[i].value, boot_options[i].title);
		}

		set_attr(0x07);
		move_cursor(x,17);
		print_("\n");
		print_banner(HELP_TEXT);
		print_("\n");

		if (sel > BASE_SEL) {
			print_banner(boot_options[sel-BASE_SEL-1].description_1);
			print_banner(boot_options[sel-BASE_SEL-1].description_2);
			print_("\n");
		} else {
			print_banner(COPYRIGHT_TEXT);
			print_("\n");
			print_banner(LINK_TEXT);
		}

		int s = read_scancode();
		if (s == 0x50) { /* DOWN */
			if (sel > BASE_SEL && sel < sel_max - 1) {
				sel = (sel + 2) % sel_max;
			} else {
				sel = (sel + 1)  % sel_max;
			}
		} else if (s == 0x48) { /* UP */
			if (sel > BASE_SEL + 1) {
				sel = (sel_max + sel - 2)  % sel_max;
			} else {
				sel = (sel_max + sel - 1)  % sel_max;
			}
		} else if (s == 0x4B) { /* LEFT */
			if (sel > BASE_SEL) {
				if ((sel - BASE_SEL) % 2) {
					sel = (sel + 1) % sel_max;
				} else {
					sel -= 1;
				}
			}
		} else if (s == 0x4D) { /* RIGHT */
			if (sel > BASE_SEL) {
				if ((sel - BASE_SEL) % 2) {
					sel = (sel + 1) % sel_max;
				} else {
					sel -= 1;
				}
			}
		} else if (s == 0x1c) {
			if (sel <= BASE_SEL) {
				boot_mode = boot_mode_names[sel].index;
				break;
			} else {
				int index = sel - BASE_SEL - 1;
				*boot_options[index].value = !*boot_options[index].value;
			}
		} else if (s >= 2 && s <= 10) {
			int i = s - 2;
			if (i <= BASE_SEL) {
				boot_mode = boot_mode_names[i].index;
				break;
			}
#if 0
		} else {
			print_hex_(s);
#endif
		}
	} while (1);
}

#ifdef EFI_PLATFORM
/* EFI boot uses simple filesystem driver */
static EFI_GUID efi_simple_file_system_protocol_guid =
	{0x0964e5b22,0x6459,0x11d2,0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b};

static EFI_GUID efi_loaded_image_protocol_guid =
	{0x5B1B31A1,0x9562,0x11d2, {0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B}};

static char space[4096];
static void boot(void) {
	UINTN count;
	EFI_HANDLE * handles;
	EFI_LOADED_IMAGE * loaded_image;
	EFI_FILE_IO_INTERFACE *efi_simple_filesystem;
	EFI_FILE *root;
	EFI_STATUS status;

	uefi_call_wrapper(ST->BootServices->SetWatchdogTimer, 4, 0, 0, 0, NULL);

	clear_();

	status = uefi_call_wrapper(ST->BootServices->HandleProtocol,
			3, ImageHandleIn, &efi_loaded_image_protocol_guid,
			(void **)&loaded_image);

	if (EFI_ERROR(status)) {
		print_("Could not obtain loaded_image_protocol\n");
		while (1) {};
	}

	print("Found loaded image...\n");

	status = uefi_call_wrapper(ST->BootServices->HandleProtocol,
			3, loaded_image->DeviceHandle, &efi_simple_file_system_protocol_guid,
			(void **)&efi_simple_filesystem);

	if (EFI_ERROR(status)) {
		print_("Could not obtain simple_file_system_protocol.\n");
		while (1) {};
	}

	status = uefi_call_wrapper(efi_simple_filesystem->OpenVolume,
			2, efi_simple_filesystem, &root);

	if (EFI_ERROR(status)) {
		print_("Could not open volume.\n");
		while (1) {};
	}

	EFI_FILE * file;

	CHAR16 kernel_name[16] = {0};
	{
		char * c = kernel_path;
		char * ascii = c;
		int i = 0;
		while (*ascii) {
			kernel_name[i] = *ascii;
			i++;
			ascii++;
		}
		if (kernel_name[i-1] == L'.') {
			kernel_name[i-1] = 0;
		}
	}

	/* Load kernel */
	status = uefi_call_wrapper(root->Open,
			5, root, &file, kernel_name, EFI_FILE_MODE_READ, 0);

	if (EFI_ERROR(status)) {
		print_("Error opening kernel.\n");
		while (1) {};
	}

	{
		EFI_PHYSICAL_ADDRESS addr = KERNEL_LOAD_START;
		EFI_ALLOCATE_TYPE type = AllocateAddress;
		EFI_MEMORY_TYPE memtype = EfiLoaderData;
		UINTN pages = 8192;
		EFI_STATUS status = 0;
		status = uefi_call_wrapper(ST->BootServices->AllocatePages, 4, type, memtype, pages, &addr);
		if (EFI_ERROR(status)) {
			print_("Could not allocate space to load boot payloads: ");
			print_hex_(status);
			print_(" ");
			print_hex_(addr);
			while (1) {};
		}
	}

	unsigned int offset = 0;
	UINTN bytes = 134217728;
	status = uefi_call_wrapper(file->Read,
			3, file, &bytes, (void *)KERNEL_LOAD_START);

	if (EFI_ERROR(status)) {
		print_("Error loading kernel.\n");
		while (1) {};
	}

	print("Read "); print_hex(bytes); print(" bytes\n");

	offset += bytes;
	while (offset % 4096) offset++;

	print("Reading modules...\n");

	char ** c = modules;
	int j = 0;
	while (*c) {
		if (strcmp(*c, "NONE")) {
			/* Try to load module */
			CHAR16 name[16] = {0};
			name[0] = L'm';
			name[1] = L'o';
			name[2] = L'd';
			name[3] = L'\\';
			char * ascii = *c;
			int i = 0;
			while (*ascii) {
				name[i+4] = (*ascii >= 'A' && *ascii <= 'Z') ? (*ascii - 'A' + 'a') : *ascii;
				name[i+5] = 0;
				i++;
				ascii++;
			}
			for (int i = 0; name[i]; ++i) {
				char c[] = {name[i], 0};
				print(c);
			}
			print("\n");
			bytes = 2097152;
_try_module_again:
			status = uefi_call_wrapper(root->Open,
					5, root, &file, name, EFI_FILE_MODE_READ, 0);
			if (!EFI_ERROR(status)) {
				status = uefi_call_wrapper(file->Read,
						3, file, &bytes, (void *)(KERNEL_LOAD_START + (uintptr_t)offset));
				if (!EFI_ERROR(status)) {
					print("Loaded "); print(*c); print("\n");
					modules_mboot[j].mod_start = KERNEL_LOAD_START + offset;
					modules_mboot[j].mod_end = KERNEL_LOAD_START + offset + bytes;
					j++;
					offset += bytes;
					while (offset % 4096) offset++;
				}
			} else {
				print_("Error opening "); print_(*c); print_(" "); print_hex_(status); print_("\n");
				while (1) { };
			}
		} else {
			multiboot_header.mods_count--;
		}
		c++;
	}

	print_("Loading ramdisk...\n");
	{
		char * c = ramdisk_path;
		CHAR16 name[16] = {0};
		char * ascii = c;
		int i = 0;
		while (*ascii) {
			name[i] = *ascii;
			i++;
			ascii++;
		}
		if (name[i-1] == L'.') {
			name[i-1] == 0;
		}
		bytes = 134217728;
		status = uefi_call_wrapper(root->Open,
				5, root, &file, name, EFI_FILE_MODE_READ, 0);
		if (!EFI_ERROR(status)) {
			uintptr_t RAMDISK_OFFSET;
			{
				EFI_PHYSICAL_ADDRESS addr;
				EFI_STATUS status = uefi_call_wrapper(ST->BootServices->AllocatePages, 4, AllocateAnyPages, EfiLoaderData, 4096, &addr);
				if (EFI_ERROR(status)) {
					print_("Failed to allocate space for ramdisk.\n");
					while (1) {};
				}
				print_("Placing ramdisk at ");
				print_hex_(addr >> 32);
				print_hex_(addr & 0xFFFFFFFF);
				print_("\n");
				RAMDISK_OFFSET = addr;
			}
			status = uefi_call_wrapper(file->Read,
					3, file, &bytes, (void*)RAMDISK_OFFSET);
			if (!EFI_ERROR(status)) {
				print_("Read "); print_hex_(bytes); print_(" bytes.\n");
				inputPtr = (uint8_t*)RAMDISK_OFFSET;
				outputPtr = (uint8_t*)KERNEL_LOAD_START + offset;
				print_("Decompressing ramdisk... ");
				if (gzip_decompress()) {
					print_("Failed?\n");
				}

				print_("Loaded "); print_(c); print_("\n");
				modules_mboot[multiboot_header.mods_count-1].mod_start = KERNEL_LOAD_START + offset;
				modules_mboot[multiboot_header.mods_count-1].mod_end = (uintptr_t)outputPtr;
				final_offset = (uint8_t *)outputPtr;
				while ((uintptr_t)final_offset & 0xfff) final_offset++;
			} else {
				print_("Failed to read ramdisk\n");
			}
		} else {
			print_("Error opening "); print_(c); print_("\n");
		}
	}

	move_kernel();


}

#else
/* BIOS boot uses native ATAPI drivers, need to find boot drive. */
static void boot(void) {

	clear_();

	multiboot_header.cmdline = (uintptr_t)cmdline;

	ata_device_detect(&ata_primary_master);
	ata_device_detect(&ata_primary_slave);
	ata_device_detect(&ata_secondary_master);
	ata_device_detect(&ata_secondary_slave);

	if (ata_primary_master.is_atapi) {
		do_it(&ata_primary_master);
	}
	if (ata_primary_slave.is_atapi) {
		do_it(&ata_primary_slave);
	}
	if (ata_secondary_master.is_atapi) {
		do_it(&ata_secondary_master);
	}
	if (ata_secondary_slave.is_atapi) {
		do_it(&ata_secondary_slave);
	}

	print_("Unable to find boot drive, can not continue.\n");
	print_("Please try GRUB or the EFI loader instead.\n");

	while (1);
}
#endif
