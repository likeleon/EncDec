// x264cli.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
extern "C" {
#include <libswscale/swscale.h>
}
#include "output.h"

/*
 x.264 encoding test with raw rgb file.
 1. Generate rgb raw file with:
    x264cli.exe gen_raw
 2. Encode using x.264
    x264.exe --input-res 320x240 --fps 10 --input-csp rgb -o outfile.mkv infile.raw
*/

typedef std::vector<char> FrameData;

static bool ReadBmp(const std::wstring& fileName, FrameData& data, size_t& width, size_t& height, size_t& bitsPerPixel)
{
	std::ifstream file(fileName.c_str(), std::ios::in | std::ios::binary);
	if (!file.is_open())
	{
		std::wcerr << L"Failed to open file: " << fileName << std::endl;
		return false;
	}

	// Read bitmap info
	file.seekg(2);
	unsigned int fileSize = 0;
	file.read(reinterpret_cast<char *>(&fileSize), 4);
	file.seekg(10);
	unsigned int dataOffset = 0;
	file.read(reinterpret_cast<char *>(&dataOffset), 4);
	file.seekg(18);
	file.read(reinterpret_cast<char *>(&width), 4);
	file.read(reinterpret_cast<char *>(&height), 4);
	file.seekg(28);
	file.read(reinterpret_cast<char *>(&bitsPerPixel), 2);
	std::wcout << fileName << L" size:" << fileSize << L" offset:" << dataOffset << L" " << width << L"x" << height << L"x" << bitsPerPixel << std::endl;

	// Read bitmap data
	const size_t dataSize = width * height * (bitsPerPixel / 8);
	data.resize(dataSize);
	file.seekg(54);
	file.read(reinterpret_cast<char *>(&data[0]), dataSize);
	file.close();

	return true;
}

static bool WriteRawFile(const std::wstring& fileName, const FrameData& data, size_t numFrame)
{
	std::ofstream file(fileName.c_str(), std::ios::out | std::ios::binary);
	if (!file.is_open())
	{
		std::wcerr << L"Failed to open file: " << fileName << std::endl;
		return false;
	}

	for (size_t frame = 0; frame < numFrame; ++frame)
	{
		file.write(&data[0], data.size());
	}

	file.close();
	return true;
}

static bool ConfigureX264(x264_param_t& param, int width, int height, cli_output_t& output, size_t numFrame, float fps)
{
	if (x264_param_default_preset(&param, "ultrafast", NULL) < 0)
	{
		std::wcerr << L"x264_param_default_preset failed" << std::endl;
		return false;
	}
	param.i_log_level = X264_LOG_DEBUG;
	param.b_vfr_input = 0;
	param.i_fps_num = 10;
	param.i_fps_den = 1;

	// Apply profile restriction
	const char* const profile = NULL;	// can be "baseline, main, high or high10"
	if (x264_param_apply_profile(&param, profile) < 0)
	{
		std::wcerr << L"x264_param_apply_profile failed" << std::endl;
		return false;
	}

	// From output
	output.configure_x264_param(&param);

	param.i_fps_num = static_cast<int>(fps * 1000 + .5);
	param.i_fps_den = 1000;
	param.i_frame_total = numFrame;
	param.i_width = width;
	param.i_height = height;
	if (!param.b_vfr_input)
	{
		param.i_timebase_num = param.i_fps_den;
		param.i_timebase_den = param.i_fps_num;
	}

	return true;
}

static int EncodeFrame(x264_t& x264, cli_output_t& output, hnd_t hout, x264_picture_t* pic, int64_t& lastDts)
{
	x264_picture_t picOut;
	x264_nal_t* nals = NULL;
	int numNal = 0;

	int frameSize = x264_encoder_encode(&x264, &nals, &numNal, pic, &picOut);
	if (frameSize < 0)
	{
		std::wcerr << L"x264_encoder_encode failed" << std::endl;
		return -1;
	}

	if (frameSize)
	{
		frameSize = output.write_frame(hout, nals[0].p_payload, frameSize, &picOut);
		lastDts = picOut.i_dts;
	}
	return frameSize;
}

static bool ConvertColorSpaceToYuv440(x264_picture_t &pic, const x264_param_t& param, size_t width, size_t height, size_t bitsPerPixel, const FrameData& frameData)
{
	HMODULE hDll = LoadLibrary(L"swscale-0.dll");
	if (!hDll)
		return false;

	typedef struct SwsContext * (*GetContextFunc)(int srcW, int srcH, enum PixelFormat srcFormat,
                                  int dstW, int dstH, enum PixelFormat dstFormat,
                                  int flags, SwsFilter *srcFilter,
                                  SwsFilter *dstFilter, const double *param);
	typedef int (*Scale)(struct SwsContext *context, const uint8_t* const srcSlice[], const int srcStride[],
              int srcSliceY, int srcSliceH, uint8_t* const dst[], const int dstStride[]);
	typedef void (*FreeContext)(struct SwsContext *swsContext);

	GetContextFunc getContextFunc = (GetContextFunc)GetProcAddress(hDll, "sws_getContext");
	Scale scaleFunc = (Scale)GetProcAddress(hDll, "sws_scale");
	FreeContext freeContextFunc = (FreeContext)GetProcAddress(hDll, "sws_freeContext");
	if (!getContextFunc || !scaleFunc || !freeContextFunc)
	{
		FreeLibrary(hDll);
		return false;
	}

	int swsFlags = 0;
	if (param.cpu & X264_CPU_ALTIVEC)
		swsFlags |= SWS_CPU_CAPS_ALTIVEC;
	if (param.cpu & X264_CPU_MMXEXT)
		swsFlags |= SWS_CPU_CAPS_MMX | SWS_CPU_CAPS_MMX2;
	swsFlags |= SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP | SWS_ACCURATE_RND;
	swsFlags |= SWS_BICUBIC;

	struct SwsContext* const swsCtx = getContextFunc(width, height, PIX_FMT_RGB24, 
		width, height, PIX_FMT_YUV420P, swsFlags, NULL, NULL, NULL);
	const uint8_t* const srcSlice[4] = {(const uint8_t *const)&frameData[0], NULL, NULL, NULL};
	const int srcStride[4] = {width * bitsPerPixel / 8, 0, 0, 0};
	scaleFunc(swsCtx, srcSlice, srcStride, 0, height, pic.img.plane, pic.img.i_stride);
	freeContextFunc(swsCtx);

	FreeLibrary(hDll);

	return true;
}

static void AllocateYuv440Pic(x264_picture_t& pic, size_t width, size_t height)
{
	const size_t numPlane = 3;
	pic.img.i_plane = numPlane;	// # planes
	pic.img.i_csp = X264_CSP_I420;
	pic.i_pts = 0;

	const float widths[numPlane] = { 1, .5, .5 };
	const float heights[numPlane] = { 1, .5, .5 };
	for (size_t i = 0; i < numPlane; ++i)
	{
		uint64_t planeSize = (uint64_t)width * height;
		planeSize = static_cast<uint64_t>(planeSize * widths[i] * heights[i]);
		pic.img.plane[i] = new uint8_t[static_cast<unsigned int>(planeSize)];
		pic.img.i_stride[i] = static_cast<int>(width * widths[i]);
	}
	for (size_t i = numPlane; i < 4; ++i)
	{
		pic.img.plane[i] = NULL;
		pic.img.i_stride[i] = 0;
	}
}

static void DeallocateYuv440Pic(x264_picture_t& pic)
{
	for (int i = 0; i < pic.img.i_plane; ++i)
		delete[] pic.img.plane[i];
	memset(pic.img.plane, 0, sizeof(uint8_t *) * 4);
	memset(pic.img.i_stride, 0, sizeof(int) * 4);
	pic.img.i_plane = 0;
	pic.img.i_csp = X264_CSP_NONE;
}

static DWORD PrintStatus(DWORD start, DWORD previous, size_t frame, int frameTotal, int64_t fileWritten, x264_param_t& param, int64_t lastTs)
{
	const DWORD now = timeGetTime();
	const DWORD updateInterval = 250000;
	if (previous && now - previous < updateInterval)
		return previous;
	const int64_t elapsed = now - start;
	const double fps = (elapsed > 0)? frame * 1000. / elapsed : 0;
	double bitrate;
	if (lastTs)
		bitrate = static_cast<double>(fileWritten) * 8 / (static_cast<double>(lastTs) * 1000 * param.i_timebase_num / param.i_timebase_den);
	else
		bitrate = static_cast<double>(fileWritten) * 8 / (static_cast<double>(1000) * param.i_fps_den / param.i_fps_num);

	if (frameTotal)
	{
		const int64_t eta = elapsed * (frameTotal - frame) / (static_cast<int64_t>(frame) * 1000);
		std::wcout << L"[" << 100. * frame / frameTotal << L"%] " <<
			frame << L"/" << frameTotal << L" frames, " <<
			fps << L" fps, " << bitrate << L" kb/s, eta " <<
			eta/3600 << L":" << (eta / 60) % 60 << L":" << eta % 60 << L"\n";
	}
	else
	{
		std::wcout << frame << " frames: " << fps << L" fps, " << bitrate << L" kb/s\n";
	}
	return now;
}

static bool EncodeX264(x264_param_t& param,
	cli_output_t& output,
	hnd_t hout,
	const FrameData& frameData,
	size_t width,
	size_t height,
	size_t bitsPerPixel,
	size_t numFrame,
	int64_t& largestPtsOut,
	int64_t& secondLargestPtsOut)
{
	x264_t* const x264 = x264_encoder_open(&param);
	if (!x264) 
	{
		std::wcerr << L"x264_encoder_open failed" << std::endl;
		return false;
	}

	bool retVal = true;
	x264_picture_t pic;
	bool picAllocated = false;
	
	x264_encoder_parameters(x264, &param);

	if (output.set_param(hout, &param) < 0)
	{
		std::wcerr << L"output::set_param failed" << std::endl;
		output.close_file(hout, 0, 0);
		retVal = false;
		goto exit;
	}

	const DWORD start = timeGetTime();
	int64_t fileWritten = 0;

	if (!param.b_repeat_headers)
	{
		// Write SPS/PPS/SEI
		x264_nal_t* headers = NULL;
		int nal = 0;
		if (x264_encoder_headers(x264, &headers, &nal) < 0)
		{
			std::wcerr << L"x264_encoder_headers failed" << std::endl;
			retVal = false;
			goto exit;
		}

		fileWritten = output.write_headers(hout, headers);
		if (fileWritten < 0)
		{
			std::wcerr << L"Error writing headers to output file" << std::endl;
			retVal = false;
			return false;
		}
	}

	// Prepare frame 
	x264_picture_init(&pic);

	AllocateYuv440Pic(pic, width, height);
	if (!ConvertColorSpaceToYuv440(pic, param, width, height, bitsPerPixel, frameData))
	{
		retVal = false;
		goto exit;
	}
	picAllocated = true;

	// Encode multiple times
	int64_t largestPts = -1, secondLargestPts = -1;
	int64_t lastDts = 0, prevDts = 0, firstDts = 0;
	size_t frameOutput = 0;
	bool encodeSuccess = true;
	DWORD previous = 0;
	for (size_t frame = 0; encodeSuccess && (frame < numFrame); ++frame)
	{
		if (!param.b_vfr_input)
			pic.i_pts = frame;

		secondLargestPts = largestPts;
		largestPts = pic.i_pts;

		prevDts = lastDts;
		const int frameSize = EncodeFrame(*x264, output, hout, &pic, lastDts);
		if (frameSize < 0)
		{
			encodeSuccess = false;	// exit the loop
			retVal = false;
		}
		else if (frameSize)
		{
			fileWritten += frameSize;
			++frameOutput;
			if (frameOutput == 1)
				firstDts = prevDts = lastDts;
		}

		if (frameOutput)
			previous = PrintStatus(start, previous, frameOutput, numFrame, fileWritten, param, 2 * lastDts - prevDts - firstDts);
	}

	// Flush delayed frames
	while (encodeSuccess && x264_encoder_delayed_frames(x264))
	{
		prevDts = lastDts;
		const int frameSize = EncodeFrame(*x264, output, hout, NULL, lastDts);
		if (frameSize < 0)
		{
			encodeSuccess = false;	// exit the loop
			retVal = false;
		}
		else if (frameSize)
		{
			fileWritten += frameSize;
			++frameOutput;
			if (frameOutput == 1)
				firstDts = prevDts = lastDts;
		}
	}

exit:
	double duration = 0.0;
	if (frameOutput == 1)
		duration = static_cast<double>(param.i_fps_den / param.i_fps_num);
	else if (!encodeSuccess)
		duration = static_cast<double>(2 * lastDts - prevDts - firstDts) * param.i_timebase_num / param.i_timebase_den;
	else
		duration = static_cast<double>(2 * largestPts - secondLargestPts) * param.i_timebase_num / param.i_timebase_den;

	const DWORD end = timeGetTime();
	if (frameOutput > 0)
	{
		const double fps = static_cast<double>(frameOutput * 1000) / 
			static_cast<double>(end - start);
		std::wcout << L"Encoded " << frameOutput << L"frames, " << fps << L" fps, " << 
			static_cast<double>(fileWritten * 8 / (1000 * duration)) << L" kb/s" << std::endl;
	}

	if (picAllocated)
	{
		DeallocateYuv440Pic(pic);
		picAllocated = false;
	}
	if (x264) 
	{
		x264_encoder_close(x264);
	}

	if (encodeSuccess)
	{
		largestPtsOut = largestPts;
		secondLargestPtsOut = secondLargestPts;
	}

	return retVal;
}

static std::string ToAnsiStr(const std::wstring& uni)
{
	if (uni.empty())
		return "";

	const int ansiLength = WideCharToMultiByte(CP_ACP, 0, uni.c_str(), uni.length(), NULL, 0, NULL, NULL);
	std::vector<char> buf(ansiLength + 1);
	WideCharToMultiByte(CP_ACP, 0, uni.c_str(), uni.length(),
		&buf[0], buf.size(), NULL, NULL);
	buf[ansiLength] = 0;
	return std::string(&buf[0]);
}

int _tmain(int argc, _TCHAR* argv[])
{
	hnd_t hout = NULL;
	cli_output_t output = mkv_output;
	
	const std::wstring bmpFileName = L"RedGreen.bmp";
	const std::wstring mkvFileName = bmpFileName + L".mkv";

	// Read bmp file
	FrameData bmpData;
	size_t width = 0, height = 0, bitsPerPixel = 0;
	if (!ReadBmp(bmpFileName, bmpData, width, height, bitsPerPixel))
		return -1;

	// Generate raw rgb file
	if (argc > 1 && std::wstring(argv[1]) == L"gen_raw")
	{
		std::wstring rawFileName = bmpFileName + L".raw";
		std::wcout << L"Generating raw file: " << rawFileName << L"..." << std::endl;
		WriteRawFile(rawFileName, bmpData, 1000);
		return 0;
	}

	const size_t numFrame = 1000;
	const float fps = 10.;

	x264_param_t param;
	if (!ConfigureX264(param, width, height, output, numFrame, fps))
		return -1;

	if (output.open_file(const_cast<char *>(ToAnsiStr(mkvFileName).c_str()), &hout, NULL) < 0)
		return -1;

	int64_t largestPts = 0, secondLargestPts = 0;
	if (!EncodeX264(param, output, hout, bmpData, width, height, bitsPerPixel, numFrame, largestPts, secondLargestPts))
	{
		output.close_file(hout, 0, 0);
		return -1;
	}

	output.close_file(hout, largestPts, secondLargestPts);
	return 0;
}