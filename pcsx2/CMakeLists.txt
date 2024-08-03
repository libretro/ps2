# All flags, libraries, etc, that are shared between PCSX2 compilation files
add_library(PCSX2_FLAGS INTERFACE)

if(DISABLE_ADVANCE_SIMD OR LTO_PCSX2_CORE)
	# Fixes issues with some compiler + linker combinations
	add_library(PCSX2 OBJECT)
else()
	add_library(PCSX2)
endif()
target_link_libraries(PCSX2 PRIVATE PCSX2_FLAGS)

target_compile_features(PCSX2_FLAGS INTERFACE cxx_std_17)
target_compile_definitions(PCSX2_FLAGS INTERFACE "${PCSX2_DEFS}")
target_compile_options(PCSX2_FLAGS INTERFACE "${PCSX2_WARNINGS}")

# Check that people use the good file
if(NOT TOP_CMAKE_WAS_SOURCED)
	message(FATAL_ERROR "
	You did not 'cmake' the good CMakeLists.txt file. Use the one in the top dir.
	It is advice to delete all wrongly generated cmake stuff => CMakeFiles & CMakeCache.txt")
endif()

if(MSVC)
	if (NOT USE_CLANG_CL AND MSVC_VERSION GREATER_EQUAL 1930)
		target_compile_options(PCSX2_FLAGS INTERFACE /fp:contract)
	endif()
	target_compile_options(PCSX2_FLAGS INTERFACE /GS-)
else()
	target_compile_options(PCSX2_FLAGS INTERFACE
		-ffp-contract=fast
		-fno-strict-aliasing
		-Wstrict-aliasing # Allow to track strict aliasing issue.
		-Wno-parentheses
		-Wno-missing-braces
		-Wno-unknown-pragmas
	)
endif()

if(CCACHE_FOUND)
	set(CommonFlags ${CommonFlags} -Werror=date-time)
endif()

if(GCC_VERSION VERSION_EQUAL "8.0" OR GCC_VERSION VERSION_GREATER "8.0")
	# gs is pretty bad at this
	target_compile_options(PCSX2_FLAGS INTERFACE -Wno-packed-not-aligned -Wno-class-memaccess)
endif()

if ("${PGO}" STREQUAL "generate")
	target_compile_options(PCSX2_FLAGS INTERFACE -fprofile-generate)
elseif("${PGO}" STREQUAL "use")
	target_compile_options(PCSX2_FLAGS INTERFACE -fprofile-use)
endif()

if(WIN32)
	set(MIN_WIN32 0x0A00)
	target_compile_definitions(PCSX2_FLAGS INTERFACE
		__SSE4_1__
		WINVER=${MIN_WIN32}
		_WIN32_WINNT=${MIN_WIN32}
		WIN32_LEAN_AND_MEAN
		WIL_SUPPRESS_EXCEPTIONS
	)
	target_include_directories(PCSX2_FLAGS INTERFACE
		../3rdparty # GL headers
	)
endif(WIN32)

# Main pcsx2 source
set(pcsx2Sources
	Cache.cpp
	COP0.cpp
	COP2.cpp
	Counters.cpp
	Dmac.cpp
	GameDatabase.cpp
	Elfheader.cpp
	FW.cpp
	FiFo.cpp
	FPU.cpp
	Gif.cpp
	Gif_Unit.cpp
	GS.cpp
	Hw.cpp
	Interpreter.cpp
	IopBios.cpp
	IopCounters.cpp
	IopDma.cpp
	IopGte.cpp
	IopHw.cpp
	IopIrq.cpp
	IopMem.cpp
	Mdec.cpp
	Memory.cpp
	MemoryCardFile.cpp
	MemoryCardProtocol.cpp
	MMI.cpp
	MTGS.cpp
	MTVU.cpp
	MultitapProtocol.cpp
	Patch.cpp
	Pcsx2Config.cpp
	PerformanceMetrics.cpp
	R3000A.cpp
	R3000AInterpreter.cpp
	R3000AOpcodeTables.cpp
	R5900.cpp
	R5900OpcodeImpl.cpp
	R5900OpcodeTables.cpp
	SaveState.cpp
	Sif.cpp
	Sif0.cpp
	Sif1.cpp
	sif2.cpp
	Sio.cpp
	SPR.cpp
	Vif0_Dma.cpp
	Vif1_Dma.cpp
	Vif1_MFIFO.cpp
	Vif.cpp
	Vif_Codes.cpp
	Vif_Transfer.cpp
	Vif_Unpack.cpp
	VirtualMemory.cpp
	vtlb.cpp
	VU0.cpp
	VUmicro.cpp
	VU0micro.cpp
	VU0microInterp.cpp
	VU1micro.cpp
	VU1microInterp.cpp
	VUmicroMem.cpp
	VUops.cpp)

# Main pcsx2 header
set(pcsx2Headers
	Cache.h
	Common.h
	Config.h
	COP0.h
	Counters.h
	Dmac.h
	GameDatabase.h
	Elfheader.h
	FW.h
	Gif.h
	Gif_Unit.h
	GS.h
	Hardware.h
	Host.h
	Hw.h
	IopBios.h
	IopCounters.h
	IopDma.h
	IopGte.h
	IopHw.h
	IopMem.h
	Mdec.h
	MTVU.h
	Memory.h
	MemoryCardFile.h
	MemoryCardProtocol.h
	MemoryTypes.h
	MultitapProtocol.h
	Patch.h
	PerformanceMetrics.h
	R3000A.h
	R5900.h
	R5900OpcodeTables.h
	SaveState.h
	ShaderCacheVersion.h
	Sif.h
	Sio.h
	SPR.h
	Vif_Dma.h
	Vif.h
	Vif_Unpack.h
	Vif_Dynarec.h
	Vif_HashBucket.h
	VirtualMemory.h
	vtlb.h
	VUmicro.h
	VUops.h)

# CDVD sources
set(pcsx2CDVDSources
	CDVD/Ps1CD.cpp
	CDVD/CDVDcommon.cpp
	CDVD/CDVD.cpp
	CDVD/CDVDisoReader.cpp
	CDVD/FlatFileReader.cpp
	CDVD/InputIsoFile.cpp
	CDVD/ChdFileReader.cpp
	CDVD/CsoFileReader.cpp
	CDVD/GzippedFileReader.cpp
	CDVD/ThreadedFileReader.cpp
	CDVD/IsoFS/IsoFile.cpp
	CDVD/IsoFS/IsoFSCDVD.cpp
	CDVD/IsoFS/IsoFS.cpp
	)

# CDVD headers
set(pcsx2CDVDHeaders
	CDVD/Ps1CD.h
	CDVD/CDVDcommon.h
	CDVD/CDVD.h
	CDVD/CDVD_internal.h
	CDVD/ChdFileReader.h
	CDVD/CsoFileReader.h
	CDVD/FlatFileReader.h
	CDVD/GzippedFileReader.h
	CDVD/ThreadedFileReader.h
	CDVD/IsoFileFormats.h
	CDVD/IsoFS/IsoDirectory.h
	CDVD/IsoFS/IsoFileDescriptor.h
	CDVD/IsoFS/IsoFile.h
	CDVD/IsoFS/IsoFSCDVD.h
	CDVD/IsoFS/IsoFS.h
	CDVD/IsoFS/SectorSource.h
	CDVD/zlib_indexed.h
	)

# SPU2 sources
set(pcsx2SPU2Sources
	SPU2/ADSR.cpp
	SPU2/DplIIdecoder.cpp
	SPU2/Dma.cpp
	SPU2/Mixer.cpp
	SPU2/spu2.cpp
	SPU2/ReadInput.cpp
	SPU2/RegTable.cpp
	SPU2/Reverb.cpp
	SPU2/ReverbResample.cpp
	SPU2/spu2freeze.cpp
	SPU2/spu2sys.cpp
)

# SPU2 headers
set(pcsx2SPU2Headers
	SPU2/defs.h
	SPU2/Dma.h
	SPU2/Global.h
	SPU2/interpolate_table.h
	SPU2/Mixer.h
	SPU2/spu2.h
	SPU2/regs.h
	SPU2/SndOut.h
	SPU2/spdif.h
)

# DEV9 sources
set(pcsx2DEV9Sources
	DEV9/AdapterUtils.cpp
	DEV9/ATA/Commands/ATA_Command.cpp
	DEV9/ATA/Commands/ATA_CmdDMA.cpp
	DEV9/ATA/Commands/ATA_CmdExecuteDeviceDiag.cpp
	DEV9/ATA/Commands/ATA_CmdNoData.cpp
	DEV9/ATA/Commands/ATA_CmdPIOData.cpp
	DEV9/ATA/Commands/ATA_CmdSMART.cpp
	DEV9/ATA/Commands/ATA_SCE.cpp
	DEV9/ATA/ATA_Info.cpp
	DEV9/ATA/ATA_State.cpp
	DEV9/ATA/ATA_Transfer.cpp
	DEV9/ATA/HddCreate.cpp
	DEV9/InternalServers/DHCP_Server.cpp
	DEV9/InternalServers/DNS_Logger.cpp
	DEV9/InternalServers/DNS_Server.cpp
	DEV9/PacketReader/ARP/ARP_Packet.cpp
	DEV9/PacketReader/ARP/ARP_PacketEditor.cpp
	DEV9/PacketReader/IP/ICMP/ICMP_Packet.cpp
	DEV9/PacketReader/IP/TCP/TCP_Options.cpp
	DEV9/PacketReader/IP/TCP/TCP_Packet.cpp
	DEV9/PacketReader/IP/UDP/DHCP/DHCP_Options.cpp
	DEV9/PacketReader/IP/UDP/DHCP/DHCP_Packet.cpp
	DEV9/PacketReader/IP/UDP/DNS/DNS_Classes.cpp
	DEV9/PacketReader/IP/UDP/DNS/DNS_Packet.cpp
	DEV9/PacketReader/IP/UDP/UDP_Packet.cpp
	DEV9/PacketReader/IP/IP_Options.cpp
	DEV9/PacketReader/IP/IP_Packet.cpp
	DEV9/PacketReader/EthernetFrame.cpp
	DEV9/PacketReader/EthernetFrameEditor.cpp
	DEV9/Sessions/BaseSession.cpp
	DEV9/Sessions/ICMP_Session/ICMP_Session.cpp
	DEV9/Sessions/TCP_Session/TCP_Session.cpp
	DEV9/Sessions/TCP_Session/TCP_Session_In.cpp
	DEV9/Sessions/TCP_Session/TCP_Session_Out.cpp
	DEV9/Sessions/UDP_Session/UDP_FixedPort.cpp
	DEV9/Sessions/UDP_Session/UDP_Session.cpp
	DEV9/smap.cpp
	DEV9/sockets.cpp
	DEV9/DEV9.cpp
	DEV9/flash.cpp
	DEV9/pcap_io.cpp
	DEV9/net.cpp
	)

# DEV9 headers
set(pcsx2DEV9Headers
	DEV9/AdapterUtils.h
	DEV9/ATA/ATA.h
	DEV9/ATA/HddCreate.h
	DEV9/DEV9.h
	DEV9/InternalServers/DHCP_Server.cpp
	DEV9/InternalServers/DNS_Logger.h
	DEV9/InternalServers/DNS_Server.h
	DEV9/net.h
	DEV9/PacketReader/ARP/ARP_Packet.h
	DEV9/PacketReader/ARP/ARP_PacketEditor.h
	DEV9/PacketReader/IP/ICMP/ICMP_Packet.h
	DEV9/PacketReader/IP/TCP/TCP_Options.h
	DEV9/PacketReader/IP/TCP/TCP_Packet.h
	DEV9/PacketReader/IP/UDP/DHCP/DHCP_Options.h
	DEV9/PacketReader/IP/UDP/DHCP/DHCP_Packet.h
	DEV9/PacketReader/IP/UDP/DNS/DNS_Classes.h
	DEV9/PacketReader/IP/UDP/DNS/DNS_Enums.h
	DEV9/PacketReader/IP/UDP/DNS/DNS_Packet.h
	DEV9/PacketReader/IP/UDP/UDP_Packet.h
	DEV9/PacketReader/IP/IP_Address.h
	DEV9/PacketReader/IP/IP_Options.h
	DEV9/PacketReader/IP/IP_Packet.h
	DEV9/PacketReader/IP/IP_Payload.h
	DEV9/PacketReader/EthernetFrame.h
	DEV9/PacketReader/EthernetFrameEditor.h
	DEV9/PacketReader/MAC_Address.h
	DEV9/PacketReader/NetLib.h
	DEV9/PacketReader/Payload.h
	DEV9/pcap_io.h
	DEV9/Sessions/BaseSession.h
	DEV9/Sessions/ICMP_Session/ICMP_Session.h
	DEV9/Sessions/TCP_Session/TCP_Session.h
	DEV9/Sessions/UDP_Session/UDP_FixedPort.h
	DEV9/Sessions/UDP_Session/UDP_BaseSession.h
	DEV9/Sessions/UDP_Session/UDP_Session.h
	DEV9/SimpleQueue.h
	DEV9/smap.h
	DEV9/sockets.h
	DEV9/ThreadSafeMap.h
	)

# USB sources
set(pcsx2USBSources
	USB/USB.cpp
	USB/deviceproxy.cpp
	USB/qemu-usb/bus.cpp
	USB/qemu-usb/core.cpp
	USB/qemu-usb/desc.cpp
	USB/qemu-usb/hid.cpp
	USB/qemu-usb/input-keymap-qcode-to-qnum.cpp
	USB/qemu-usb/usb-ohci.cpp
	USB/shared/ringbuffer.cpp
	USB/usb-eyetoy/jo_mpeg.cpp
	USB/usb-eyetoy/usb-eyetoy-webcam.cpp
	USB/usb-hid/usb-hid.cpp
	USB/usb-lightgun/guncon2.cpp
	USB/usb-mic/usb-headset.cpp
	USB/usb-mic/usb-mic-logitech.cpp
	USB/usb-mic/usb-mic-singstar.cpp
	USB/usb-msd/usb-msd.cpp
	USB/usb-pad/lg/lg_ff.cpp
	USB/usb-pad/usb-pad-ff.cpp
	USB/usb-pad/usb-pad.cpp
	USB/usb-pad/usb-seamic.cpp
	USB/usb-printer/usb-printer.cpp
)

# USB headers
set(pcsx2USBHeaders
	USB/USB.h
	USB/deviceproxy.h
	USB/qemu-usb/USBinternal.h
	USB/qemu-usb/desc.h
	USB/qemu-usb/hid.h
	USB/qemu-usb/input-keymap.h
	USB/qemu-usb/queue.h
	USB/qemu-usb/qusb.h
	USB/shared/ringbuffer.h
	USB/usb-eyetoy/jo_mpeg.h
	USB/usb-eyetoy/ov519.h
	USB/usb-eyetoy/usb-eyetoy-webcam.h
	USB/usb-eyetoy/videodev.h
	USB/usb-hid/usb-hid.h
	USB/usb-lightgun/guncon2.h
	USB/usb-mic/audio.h
	USB/usb-mic/audiodev-noop.h
	USB/usb-mic/audiodev.h
	USB/usb-mic/usb-headset.h
	USB/usb-mic/usb-mic-singstar.h
	USB/usb-msd/usb-msd.h
	USB/usb-pad/lg/lg_ff.h
	USB/usb-pad/usb-pad.h
	USB/usb-printer/usb-printer.h
)

# Host PAD
set(pcsx2PADSources
	PAD/PAD.cpp
)
set(pcsx2PADHeaders
	PAD/PAD.h
)

# GS sources
set(pcsx2GSSourcesUnshared
	GS/GSBlock.cpp
	GS/GSLocalMemoryMultiISA.cpp
	GS/GSXXH.cpp
	GS/Renderers/Common/GSVertexTraceFMM.cpp
	GS/Renderers/HW/GSRendererHWMultiISA.cpp
	GS/Renderers/SW/GSDrawScanline.cpp
	GS/Renderers/SW/GSDrawScanlineCodeGenerator.cpp
	GS/Renderers/SW/GSDrawScanlineCodeGenerator.all.cpp
	GS/Renderers/SW/GSRasterizer.cpp
	GS/Renderers/SW/GSRendererSW.cpp
	GS/Renderers/SW/GSSetupPrimCodeGenerator.cpp
	GS/Renderers/SW/GSSetupPrimCodeGenerator.all.cpp
)

set(pcsx2GSSources
	GS/GS.cpp
	GS/GSClut.cpp
	GS/GSDrawingContext.cpp
	GS/GSLocalMemory.cpp
	GS/GSRingHeap.cpp
	GS/GSState.cpp
	GS/GSTables.cpp
	GS/GSUtil.cpp
	GS/GSVector.cpp
	GS/MultiISA.cpp
	GS/Renderers/Common/GSDevice.cpp
	GS/Renderers/Common/GSDirtyRect.cpp
	GS/Renderers/Common/GSFunctionMap.cpp
	GS/Renderers/Common/GSRenderer.cpp
	GS/Renderers/Common/GSTexture.cpp
	GS/Renderers/Common/GSVertexTrace.cpp
	GS/Renderers/HW/GSHwHack.cpp
	GS/Renderers/HW/GSRendererHW.cpp
	GS/Renderers/HW/GSTextureCache.cpp
	GS/Renderers/HW/GSTextureReplacementLoaders.cpp
	GS/Renderers/HW/GSTextureReplacements.cpp
	GS/Renderers/SW/GSTextureCacheSW.cpp
	)

# GS headers
set(pcsx2GSHeaders
	GS/GSAlignedClass.h
	GS/GSBlock.h
	GS/GSClut.h
	GS/GSDrawingContext.h
	GS/GSDrawingEnvironment.h
	GS/GSExtra.h
	GS/GSRegs.h
	GS/GS.h
	GS/GSLocalMemory.h
	GS/GSRingHeap.h
	GS/GSState.h
	GS/GSTables.h
	GS/GSUtil.h
	GS/GSVector.h
	GS/GSVector4.h
	GS/GSVector4i.h
	GS/GSVector8.h
	GS/GSVector8i.h
	GS/GSXXH.h
	GS/MultiISA.h
	GS/Renderers/Common/GSDevice.h
	GS/Renderers/Common/GSDirtyRect.h
	GS/Renderers/Common/GSFastList.h
	GS/Renderers/Common/GSFunctionMap.h
	GS/Renderers/Common/GSRenderer.h
	GS/Renderers/Common/GSTexture.h
	GS/Renderers/Common/GSVertex.h
	GS/Renderers/Common/GSVertexTrace.h
	GS/Renderers/HW/GSHwHack.h
	GS/Renderers/HW/GSRendererHW.h
	GS/Renderers/HW/GSTextureCache.h
	GS/Renderers/HW/GSTextureReplacements.h
	GS/Renderers/HW/GSVertexHW.h
	GS/Renderers/SW/GSDrawScanlineCodeGenerator.h
	GS/Renderers/SW/GSDrawScanlineCodeGenerator.all.h
	GS/Renderers/SW/GSDrawScanline.h
	GS/Renderers/SW/GSNewCodeGenerator.h
	GS/Renderers/SW/GSRasterizer.h
	GS/Renderers/SW/GSRendererSW.h
	GS/Renderers/SW/GSScanlineEnvironment.h
	GS/Renderers/SW/GSSetupPrimCodeGenerator.h
	GS/Renderers/SW/GSSetupPrimCodeGenerator.all.h
	GS/Renderers/SW/GSTextureCacheSW.h
	GS/Renderers/SW/GSVertexSW.h
	)

if(USE_OPENGL)
	list(APPEND pcsx2GSSources
		GS/Renderers/OpenGL/GLState.cpp
		GS/Renderers/OpenGL/GSDeviceOGL.cpp
		GS/Renderers/OpenGL/GSTextureOGL.cpp
		GS/Renderers/OpenGL/GLContext.cpp
		GS/Renderers/OpenGL/GLProgram.cpp
		GS/Renderers/OpenGL/GLShaderCache.cpp
		GS/Renderers/OpenGL/GLStreamBuffer.cpp
	)
	list(APPEND pcsx2GSHeaders
		GS/Renderers/OpenGL/GLState.h
		GS/Renderers/OpenGL/GSDeviceOGL.h
		GS/Renderers/OpenGL/GSTextureOGL.h
		GS/Renderers/OpenGL/GLContext.h
		GS/Renderers/OpenGL/GLProgram.h
		GS/Renderers/OpenGL/GLShaderCache.h
		GS/Renderers/OpenGL/GLStreamBuffer.h
	)
	target_link_libraries(PCSX2_FLAGS INTERFACE glad)
endif()

if(USE_VULKAN)
	list(APPEND pcsx2GSSources
		GS/Renderers/Vulkan/GSDeviceVK.cpp
		GS/Renderers/Vulkan/GSTextureVK.cpp
		GS/Renderers/Vulkan/VKShaderCache.cpp
		GS/Renderers/Vulkan/VKLoader.cpp
		GS/Renderers/Vulkan/VKStreamBuffer.cpp
		GS/Renderers/Vulkan/VKBuilders.cpp
		GS/Renderers/Vulkan/vk_mem_alloc.cpp
	)
	list(APPEND pcsx2GSHeaders
		GS/Renderers/Vulkan/GSDeviceVK.h
		GS/Renderers/Vulkan/GSTextureVK.h
		GS/Renderers/Vulkan/VKBuilders.h
		GS/Renderers/Vulkan/VKStreamBuffer.h
		GS/Renderers/Vulkan/VKShaderCache.h
		GS/Renderers/Vulkan/VKEntryPoints.h
		GS/Renderers/Vulkan/VKLoader.h
		GS/Renderers/Vulkan/VKLoaderPlatformDefines.h
	)

	set(PARALLEL_GS_STANDALONE ON CACHE BOOL "" FORCE)
	set(GRANITE_POSITION_INDEPENDENT ON CACHE BOOL "" FORCE)
	message("Adding support for paraLLEl-GS")
	add_subdirectory(GS/parallel-gs EXCLUDE_FROM_ALL)
	target_link_libraries(PCSX2_FLAGS INTERFACE parallel-gs)
	target_compile_definitions(PCSX2_FLAGS INTERFACE HAVE_PARALLEL_GS)
	list(APPEND pcsx2GSSources GS/Renderers/parallel-gs/GSRendererPGS.cpp)
	list(APPEND pcsx2GSHeaders GS/Renderers/parallel-gs/GSRendererPGS.h)

	target_link_libraries(common PUBLIC
		Vulkan-Headers glslang
	)
	if(APPLE)
		# Needed for Metal surface creation.
		target_compile_options(common PRIVATE -fobjc-arc)
		target_link_options(common PRIVATE -fobjc-link-runtime)
	endif()
endif()

if(WIN32)
	list(APPEND pcsx2DEV9Sources
		DEV9/Win32/pcap_io_win32.cpp
		DEV9/Win32/tap-win32.cpp
	)
	list(APPEND pcsx2DEV9Headers
		DEV9/Win32/pcap_io_win32_funcs.h
		DEV9/Win32/tap.h
	)

	list(APPEND pcsx2USBSources
		USB/usb-eyetoy/cam-windows.cpp
	)
	list(APPEND pcsx2USBHeaders
		USB/usb-eyetoy/cam-windows.h
	)

target_compile_options(PCSX2_FLAGS INTERFACE "-DHAVE_D3D11")
target_compile_options(PCSX2_FLAGS INTERFACE "-DHAVE_D3D12")

	list(APPEND pcsx2GSSources
		GS/Renderers/DX11/D3D.cpp
		GS/Renderers/DX11/GSDevice11.cpp
		GS/Renderers/DX11/GSTexture11.cpp
		GS/Renderers/DX11/D3D11ShaderCache.cpp
		GS/Renderers/DX12/GSDevice12.cpp
		GS/Renderers/DX12/GSTexture12.cpp
		GS/Renderers/DX12/D3D12Builders.cpp
		GS/Renderers/DX12/D3D12DescriptorHeapManager.cpp
		GS/Renderers/DX12/D3D12ShaderCache.cpp
		GS/Renderers/DX12/D3D12StreamBuffer.cpp
	)
	list(APPEND pcsx2GSHeaders
		GS/Renderers/DX11/D3D.h
		GS/Renderers/DX11/GSDevice11.h
		GS/Renderers/DX11/GSTexture11.h
		GS/Renderers/DX11/D3D11ShaderCache.h
		GS/Renderers/DX12/GSDevice12.h
		GS/Renderers/DX12/GSTexture12.h
		GS/Renderers/DX12/D3D12Builders.h
		GS/Renderers/DX12/D3D12DescriptorHeapManager.h
		GS/Renderers/DX12/D3D12ShaderCache.h
		GS/Renderers/DX12/D3D12StreamBuffer.h
	)
elseif(Linux)
	list(APPEND pcsx2USBSources
		USB/usb-eyetoy/cam-linux.cpp
	)
	list(APPEND pcsx2USBHeaders
		USB/usb-eyetoy/cam-linux.h
	)
else()
	list(APPEND pcsx2USBSources
		USB/usb-eyetoy/cam-noop.cpp
	)
endif()

# IPU sources
set(pcsx2IPUSources
	IPU/IPU.cpp
	IPU/IPU_Fifo.cpp
	IPU/IPUdma.cpp
)

set(pcsx2IPUSourcesUnshared
	IPU/IPU_MultiISA.cpp
	IPU/IPUdither.cpp
	IPU/yuv2rgb.cpp
)

# IPU headers
set(pcsx2IPUHeaders
	IPU/IPU.h
	IPU/IPU_Fifo.h
	IPU/IPU_MultiISA.h
	IPU/IPUdma.h
	IPU/mpeg2_vlc.h
	IPU/yuv2rgb.h
)

if(DISABLE_ADVANCE_SIMD)
	target_compile_definitions(PCSX2 PUBLIC MULTI_ISA_SHARED_COMPILATION)
	if(USE_GCC)
		target_link_options(PCSX2_FLAGS INTERFACE -Wno-odr)
	endif()
	if(WIN32)
		set(compile_options_avx2 /arch:AVX2)
		set(compile_options_avx  /arch:AVX)
	elseif(USE_GCC)
		# GCC can't inline into multi-isa functions if we use march and mtune, but can if we use feature flags
		set(compile_options_avx2 -msse4.1 -mavx -mavx2 -mbmi -mbmi2 -mfma)
		set(compile_options_avx  -msse4.1 -mavx)
		set(compile_options_sse4 -msse4.1)
	else()
		set(compile_options_avx2 -march=haswell -mtune=haswell)
		set(compile_options_avx  -march=sandybridge -mtune=sandybridge)
		set(compile_options_sse4 -msse4.1 -mtune=nehalem)
	endif()
	# ODR violation time!
	# Everything would be fine if we only defined things in cpp files, but C++ tends to like inline functions (STL anyone?)
	# Each ISA will bring with it its own copies of these inline header functions, and the linker gets to choose whichever one it wants!  Not fun if the linker chooses the avx2 version and uses it with everything
	# Thankfully, most linkers don't choose at random.  When presented with a bunch of .o files, most linkers seem to choose the first implementation they see, so make sure you order these from oldest to newest
	# Note: ld64 (macOS's linker) does not act the same way when presented with .a files, unless linked with `-force_load` (cmake WHOLE_ARCHIVE).
	set(is_first_isa "1")
	foreach(isa "sse4" "avx" "avx2")
		add_library(GS-${isa} STATIC ${pcsx2GSSourcesUnshared} ${pcsx2IPUSourcesUnshared})
		target_link_libraries(GS-${isa} PRIVATE PCSX2_FLAGS)
		target_compile_definitions(GS-${isa} PRIVATE MULTI_ISA_UNSHARED_COMPILATION=isa_${isa} MULTI_ISA_IS_FIRST=${is_first_isa} ${pcsx2_defs_${isa}})
		target_compile_options(GS-${isa} PRIVATE ${compile_options_${isa}})
		if (${CMAKE_VERSION} VERSION_GREATER_EQUAL 3.24)
			target_link_libraries(PCSX2 PRIVATE $<LINK_LIBRARY:WHOLE_ARCHIVE,GS-${isa}>)
		elseif(APPLE)
			message(FATAL_ERROR "MacOS builds with DISABLE_ADVANCE_SIMD=ON require CMake 3.24")
		else()
			target_link_libraries(PCSX2 PRIVATE GS-${isa})
		endif()
		set(is_first_isa "0")
	endforeach()
else()
	list(APPEND pcsx2GSSources ${pcsx2GSSourcesUnshared})
	list(APPEND pcsx2IPUSources ${pcsx2IPUSourcesUnshared})
endif()

# DebugTools sources
set(pcsx2DebugToolsSources
	DebugTools/DebugInterface.cpp
	DebugTools/MIPSAnalyst.cpp
	DebugTools/SymbolMap.cpp
	)

# DebugTools headers
set(pcsx2DebugToolsHeaders
	DebugTools/DebugInterface.h
	DebugTools/MIPSAnalyst.h
	DebugTools/SymbolMap.h
	)

list(APPEND pcsx2FrontendSources
	Frontend/HostSettings.cpp
	Frontend/LayeredSettingsInterface.cpp
	VMManager.cpp
)
list(APPEND pcsx2FrontendHeaders
	Frontend/LayeredSettingsInterface.h
	VMManager.h)

# Linux sources
set(pcsx2LinuxSources
	)

set(pcsx2OSXSources
	)

set(pcsx2FreeBSDSources
	)

# Linux headers
set(pcsx2LinuxHeaders
	)

# ps2 sources
set(pcsx2ps2Sources
	ps2/BiosTools.cpp
	ps2/pgif.cpp
	ps2/Iop/IopHwRead.cpp
	ps2/Iop/IopHwWrite.cpp
	ps2/Iop/PsxBios.cpp)

# ps2 headers
set(pcsx2ps2Headers
	ps2/BiosTools.h
	ps2/pgif.h
	ps2/HwInternal.h
	ps2/Iop/IopHw_Internal.h)

# Windows sources
set(pcsx2WindowsSources
)

# x86 sources
set(pcsx2x86Sources
	x86/BaseblockEx.cpp
	x86/iCOP0.cpp
	x86/iCore.cpp
	x86/iFPU.cpp
	x86/iFPUd.cpp
	x86/iMMI.cpp
	x86/iR3000A.cpp
	x86/iR3000Atables.cpp
	x86/iR5900Analysis.cpp
	x86/iR5900Misc.cpp
	x86/ix86-32/iCore-32.cpp
	x86/ix86-32/iR5900-32.cpp
	x86/ix86-32/iR5900Arit.cpp
	x86/ix86-32/iR5900AritImm.cpp
	x86/ix86-32/iR5900Branch.cpp
	x86/ix86-32/iR5900Jump.cpp
	x86/ix86-32/iR5900LoadStore.cpp
	x86/ix86-32/iR5900Move.cpp
	x86/ix86-32/iR5900MultDiv.cpp
	x86/ix86-32/iR5900Shift.cpp
	x86/ix86-32/iR5900Templates.cpp
	x86/ix86-32/recVTLB.cpp
	x86/newVif_Dynarec.cpp
	x86/newVif_UnpackSSE.cpp
	)

# x86 headers
set(pcsx2x86Headers
	x86/BaseblockEx.h
	x86/iCOP0.h
	x86/iCore.h
	x86/iFPU.h
	x86/iMMI.h
	x86/iR3000A.h
	x86/iR5900Arit.h
	x86/iR5900AritImm.h
	x86/iR5900Branch.h
	x86/iR5900.h
	x86/iR5900Analysis.h
	x86/iR5900Jump.h
	x86/iR5900LoadStore.h
	x86/iR5900Move.h
	x86/iR5900MultDiv.h
	x86/iR5900Shift.h
	x86/microVU_Alloc.inl
	x86/microVU_Analyze.inl
	x86/microVU_Branch.inl
	x86/microVU_Clamp.inl
	x86/microVU_Compile.inl
	x86/microVU.cpp
	x86/microVU_Execute.inl
	x86/microVU_Flags.inl
	x86/microVU.h
	x86/microVU_IR.h
	x86/microVU_Lower.inl
	x86/microVU_Macro.inl
	x86/microVU_Misc.h
	x86/microVU_Misc.inl
	x86/microVU_Tables.inl
	x86/microVU_Upper.inl
	x86/newVif.h
	x86/newVif_UnpackSSE.h
	)

set(pcsx2DEV9Sources)
set(pcsx2DEV9Headers)
set(pcsx2USBSources)
set(pcsx2USBHeaders)
set(pcsx2ZipToolsSources)
set(pcsx2ZipToolsHeaders)

# These ones benefit a lot from LTO
set(pcsx2LTOSources
	${pcsx2Sources}
	${pcsx2Headers}
	${pcsx2IPUSources}
	${pcsx2IPUHeaders}
	${pcsx2x86Sources}
	${pcsx2x86Headers}
	${pcsx2SPU2Sources}
	${pcsx2SPU2Headers}
	${pcsx2GSSources}
	${pcsx2GSHeaders}
)

if(LTO_PCSX2_CORE)
	add_library(PCSX2_LTO ${pcsx2LTOSources})
	if (WIN32)
		target_compile_definitions(PCSX2_LTO PRIVATE _M_SSE=0x401)
	endif()
	if (DISABLE_ADVANCE_SIMD)
		target_compile_definitions(PCSX2_LTO PRIVATE MULTI_ISA_SHARED_COMPILATION)
	endif()
	target_link_libraries(PCSX2_LTO PRIVATE PCSX2_FLAGS)
	target_link_libraries(PCSX2 PRIVATE PCSX2_LTO)
	set_target_properties(PCSX2_LTO PROPERTIES INTERPROCEDURAL_OPTIMIZATION true)
	fixup_file_properties(PCSX2_LTO)
else()
	target_sources(PCSX2 PRIVATE ${pcsx2LTOSources})
endif()

# common Sources
target_sources(PCSX2 PRIVATE
	${pcsx2CDVDSources}
	${pcsx2CDVDHeaders}
	${pcsx2DEV9Sources}
	${pcsx2DEV9Headers}
	${pcsx2PADHeaders}
	${pcsx2PADSources}
	${pcsx2DebugToolsSources}
	${pcsx2DebugToolsHeaders}
	${pcsx2FrontendSources}
	${pcsx2FrontendHeaders}
	${pcsx2ps2Sources}
	${pcsx2ps2Headers}
)

# platform sources
# Linux
if(Linux)
	target_sources(PCSX2 PRIVATE
		${pcsx2LinuxSources}
		${pcsx2LinuxHeaders}
		)

	target_link_libraries(PCSX2_FLAGS INTERFACE
	)
endif()

# Windows
if(Windows)
	target_sources(PCSX2 PRIVATE
		${pcsx2WindowsSources}
	)
endif()

target_sources(PCSX2 PRIVATE ${pcsx2USBSources} ${pcsx2USBHeaders})

# MacOSX/BSD
if(UNIX AND NOT Linux)
	if(APPLE)
		target_sources(PCSX2 PRIVATE
			${pcsx2OSXSources})
	else()
		target_sources(PCSX2 PRIVATE
			${pcsx2FreeBSDSources})
	endif()
	target_sources(PCSX2 PRIVATE
		${pcsx2LinuxHeaders})
endif()

target_link_libraries(PCSX2_FLAGS INTERFACE
	common
	fmt::fmt
	ryml
	chdr-static
	libzip::zip
	cpuinfo
	ZLIB::ZLIB
	LZ4::LZ4
	PNG::PNG
	Zstd::Zstd
	${LIBC_LIBRARIES}
)

if(WIN32)
	target_link_libraries(PCSX2_FLAGS INTERFACE
		WIL::WIL
		D3D12MemAlloc
		d3dcompiler.lib
		d3d11.lib
		d3d12.lib
		dxgi.lib
		opengl32.lib
		OneCore.lib
	)
elseif(APPLE)
else()
	target_link_libraries(PCSX2_FLAGS INTERFACE
		OpenGL::GL
	)
endif()

# additonal include directories
target_include_directories(PCSX2_FLAGS INTERFACE
	.
	x86
	${CMAKE_BINARY_DIR}/pcsx2
	${CMAKE_BINARY_DIR}/common/include/
	"${CMAKE_SOURCE_DIR}/3rdparty/xbyak/"
	"${FFMPEG_INCLUDE_DIRS}"
)

if (NOT APPLE)
	set_target_properties(PCSX2 PROPERTIES
		OUTPUT_NAME pcsx2
	)
endif()

fixup_file_properties(PCSX2)

if (APPLE)
	find_library(QUARTZCORE_LIBRARY QuartzCore)
	target_link_libraries(PCSX2_FLAGS INTERFACE ${METAL_LIBRARY} ${QUARTZCORE_LIBRARY})

	# MacOS defaults to having a maximum protection of the __DATA segment of rw (non-executable)
	# We have a bunch of page-sized arrays in bss that we use for jit
	# Obviously not being able to make those arrays executable would be a problem
	target_link_options(PCSX2_FLAGS INTERFACE -Wl,-segprot,__DATA,rwx,rw)
endif()

set_property(GLOBAL PROPERTY PCSX2_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

source_groups_from_vcxproj_filters(pcsx2core.vcxproj.filters)

# Unix-only files aren't in the vcxproj.filters
source_group(System/Ps2/DEV9 REGULAR_EXPRESSION DEV9/*)
source_group(System/Ps2/PAD FILES ${pcsx2PADSources} ${pcsx2PADHeaders})
source_group(System/Ps2/SPU2 REGULAR_EXPRESSION SPU2/*)
source_group(System/Ps2/USB REGULAR_EXPRESSION USB/*)
