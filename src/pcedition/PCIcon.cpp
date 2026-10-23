#include "pcedition/PCIcon.h"

#if BOOST_OS_WINDOWS
#include <windows.h>
#endif
#include <fstream>

namespace pce::Icon
{
	namespace
	{
		struct Image
		{
			int w = 0, h = 0;
			std::vector<uint8> rgba; // top-down, straight alpha
		};

		// uncompressed true colour TGA (what the Wii U meta folder holds): 24 or 32 bit
		bool ReadTga(const std::vector<uint8>& d, Image& img)
		{
			if (d.size() < 18 || d[1] != 0 || d[2] != 2)
				return false;
			const int w = d[12] | (d[13] << 8), h = d[14] | (d[15] << 8), bpp = d[16];
			const size_t bytes = bpp / 8, start = 18 + (size_t)d[0];
			if (w < 16 || h < 16 || w > 1024 || h > 1024 || (bpp != 24 && bpp != 32) || d.size() < start + (size_t)w * h * bytes)
				return false;
			const bool topDown = (d[17] & 0x20) != 0;
			img.w = w;
			img.h = h;
			img.rgba.resize((size_t)w * h * 4);
			for (int y = 0; y < h; y++)
			{
				const uint8* src = d.data() + start + (size_t)(topDown ? y : h - 1 - y) * w * bytes;
				uint8* dst = img.rgba.data() + (size_t)y * w * 4;
				for (int x = 0; x < w; x++, src += bytes, dst += 4)
				{
					dst[0] = src[2];
					dst[1] = src[1];
					dst[2] = src[0];
					dst[3] = bytes == 4 ? src[3] : 255;
				}
			}
			return true;
		}

		// bilinear sample at a position in source pixels (pixel centres at +0.5), premultiplied result
		void Sample(const Image& img, float u, float v, float out[4])
		{
			u = std::clamp(u - 0.5f, 0.0f, (float)img.w - 1.0f);
			v = std::clamp(v - 0.5f, 0.0f, (float)img.h - 1.0f);
			const int x0 = (int)u, y0 = (int)v, x1 = std::min(x0 + 1, img.w - 1), y1 = std::min(y0 + 1, img.h - 1);
			const float fx = u - (float)x0, fy = v - (float)y0;
			const float wgt[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
			const uint8* p[4] = {&img.rgba[((size_t)y0 * img.w + x0) * 4], &img.rgba[((size_t)y0 * img.w + x1) * 4],
								 &img.rgba[((size_t)y1 * img.w + x0) * 4], &img.rgba[((size_t)y1 * img.w + x1) * 4]};
			out[0] = out[1] = out[2] = out[3] = 0.0f;
			for (int i = 0; i < 4; i++)
			{
				const float a = (float)p[i][3] / 255.0f * wgt[i];
				out[0] += (float)p[i][0] * a;
				out[1] += (float)p[i][1] * a;
				out[2] += (float)p[i][2] * a;
				out[3] += a;
			}
		}

		// One icon size, rendered on its own from the source (supersampled): rounded corners, and from 24 px upwards a
		// thin dark outline that keeps the shape readable on light backgrounds. Result: top-down BGRA, straight alpha.
		std::vector<uint8> Render(const Image& src, int size)
		{
			const int ss = size < 32 ? 8 : 4;
			const float big = (float)(size * ss);
			const float radius = big * (size >= 32 ? 0.16f : 0.12f);
			const float outline = size >= 24 ? std::max((float)ss, big * 0.012f) : 0.0f;
			const float half = big * 0.5f;
			std::vector<uint8> out((size_t)size * size * 4);
			for (int y = 0; y < size; y++)
			{
				for (int x = 0; x < size; x++)
				{
					float acc[4] = {0, 0, 0, 0};
					for (int sy = 0; sy < ss; sy++)
					{
						for (int sx = 0; sx < ss; sx++)
						{
							const float px = (float)(x * ss + sx) + 0.5f, py = (float)(y * ss + sy) + 0.5f;
							// signed distance to the rounded square (negative inside)
							const float qx = std::fabs(px - half) - (half - radius), qy = std::fabs(py - half) - (half - radius);
							const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
							const float dist = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
							if (dist > 0.0f)
								continue;
							float c[4];
							Sample(src, px / big * (float)src.w, py / big * (float)src.h, c);
							// the art is opaque: only the shape decides the transparency
							if (c[3] > 0.0f)
							{
								c[0] /= c[3];
								c[1] /= c[3];
								c[2] /= c[3];
							}
							c[3] = 1.0f;
							if (-dist < outline)
							{
								const float k = 150.0f / 255.0f;
								c[0] = c[0] * (1 - k) + 10.0f * k;
								c[1] = c[1] * (1 - k) + 14.0f * k;
								c[2] = c[2] * (1 - k) + 20.0f * k;
							}
							for (int i = 0; i < 4; i++)
								acc[i] += c[i];
						}
					}
					uint8* d = &out[((size_t)y * size + x) * 4];
					const float n = (float)(ss * ss);
					if (acc[3] > 0.0f)
					{
						d[0] = (uint8)std::clamp(acc[2] / acc[3] + 0.5f, 0.0f, 255.0f);
						d[1] = (uint8)std::clamp(acc[1] / acc[3] + 0.5f, 0.0f, 255.0f);
						d[2] = (uint8)std::clamp(acc[0] / acc[3] + 0.5f, 0.0f, 255.0f);
					}
					d[3] = (uint8)std::clamp(acc[3] / n * 255.0f + 0.5f, 0.0f, 255.0f);
				}
			}
			return out;
		}

		template<typename T>
		void Put(std::vector<uint8>& v, T value)
		{
			const uint8* p = (const uint8*)&value;
			v.insert(v.end(), p, p + sizeof(T));
		}
	}

	bool WriteFromTga(const std::vector<uint8>& tga, const fs::path& icoPath)
	{
		Image src;
		if (!ReadTga(tga, src))
			return false;
		static const int kSizes[] = {256, 128, 96, 64, 48, 40, 32, 24, 20, 16};
		const size_t count = std::size(kSizes);
		// ICONDIR + ICONDIRENTRY[count], then one 32 bit DIB (colour bottom-up + 1 bit mask) per size
		std::vector<uint8> file, images;
		Put<uint16>(file, 0);
		Put<uint16>(file, 1);
		Put<uint16>(file, (uint16)count);
		for (int size : kSizes)
		{
			const std::vector<uint8> px = Render(src, size);
			const size_t maskRow = ((size_t)size + 31) / 32 * 4;
			std::vector<uint8> dib;
			Put<uint32>(dib, 40);
			Put<sint32>(dib, size);
			Put<sint32>(dib, size * 2);
			Put<uint16>(dib, 1);
			Put<uint16>(dib, 32);
			Put<uint32>(dib, 0);
			Put<uint32>(dib, (uint32)(px.size() + maskRow * size));
			Put<uint32>(dib, 0);
			Put<uint32>(dib, 0);
			Put<uint32>(dib, 0);
			Put<uint32>(dib, 0);
			for (int y = size - 1; y >= 0; y--)
				dib.insert(dib.end(), px.begin() + (size_t)y * size * 4, px.begin() + (size_t)(y + 1) * size * 4);
			for (int y = size - 1; y >= 0; y--)
			{
				std::vector<uint8> row(maskRow, 0);
				for (int x = 0; x < size; x++)
				{
					if (px[((size_t)y * size + x) * 4 + 3] == 0)
						row[x / 8] |= (uint8)(0x80 >> (x % 8));
				}
				dib.insert(dib.end(), row.begin(), row.end());
			}
			Put<uint8>(file, (uint8)(size >= 256 ? 0 : size));
			Put<uint8>(file, (uint8)(size >= 256 ? 0 : size));
			Put<uint8>(file, 0);
			Put<uint8>(file, 0);
			Put<uint16>(file, 1);
			Put<uint16>(file, 32);
			Put<uint32>(file, (uint32)dib.size());
			Put<uint32>(file, (uint32)(6 + 16 * count + images.size()));
			images.insert(images.end(), dib.begin(), dib.end());
		}
		file.insert(file.end(), images.begin(), images.end());
		std::error_code ec;
		fs::create_directories(icoPath.parent_path(), ec);
		fs::path tmp = icoPath;
		tmp += L".tmp";
		{
			std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
			if (!o)
				return false;
			o.write((const char*)file.data(), (std::streamsize)file.size());
			if (!o)
				return false;
		}
		fs::rename(tmp, icoPath, ec);
		if (ec)
		{
			fs::remove(icoPath, ec);
			fs::rename(tmp, icoPath, ec);
		}
		return !ec;
	}

	bool ApplyToWindow(void* hwnd, const fs::path& icoPath)
	{
#if BOOST_OS_WINDOWS
		std::error_code ec;
		if (!hwnd || !fs::exists(icoPath, ec))
			return false;
		const HICON big = (HICON)LoadImageW(nullptr, icoPath.c_str(), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE);
		const HICON small = (HICON)LoadImageW(nullptr, icoPath.c_str(), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE);
		if (!big || !small)
			return false;
		// the icons live as long as the window (process)
		SendMessageW((HWND)hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
		SendMessageW((HWND)hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small);
		return true;
#else
		(void)hwnd;
		(void)icoPath;
		return false;
#endif
	}
}
