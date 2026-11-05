#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPce.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"

#include "util/highresolutiontimer/HighResolutionTimer.h"
#include "util/containers/robin_hood.h"

namespace VulkanPce
{
	namespace
	{
		// a draw: same in two frames for the same object (several objects can share one: told apart by their uniforms)
		struct Key
		{
			uint64 vs = 0, ps = 0;
			MPTR indices = 0, vertices = 0;
			uint32 count = 0, instances = 0, baseVertex = 0;
			bool operator==(const Key&) const = default;
		};

		struct KeyHash
		{
			size_t operator()(const Key& k) const
			{
				uint64 h = k.vs * 0x9E3779B97F4A7C15ull ^ k.ps;
				h = std::rotl<uint64>(h, 17) ^ ((uint64)k.indices << 32 | k.vertices);
				h = std::rotl<uint64>(h, 13) ^ ((uint64)k.count << 40 ^ (uint64)k.instances << 20 ^ k.baseVertex);
				return (size_t)(h * 0xBF58476D1CE4E5B9ull);
			}
		};

		// the words of a key's uniforms in which its objects differ (e.g. their matrices), learned once from a frame with
		// several of them; a draw keeps its values of them (signature), the next frame compares signatures
		constexpr uint32 kSigWords = 16;

		struct KeyWords
		{
			uint16 word[kSigWords]{};
			uint32 count = 0;
		};

		robin_hood::unordered_node_map<Key, KeyWords, KeyHash> s_keyWords;

		// the uniforms a draw used: the remapped array (offset: where the draw's uniform var block put it in the uniform ring
		// buffer, which keeps it for the next frame), or its uniform buffers one after the other (offset: in Frame::data,
		// where each one is: Frame::slots)
		struct Record
		{
			uint32 offset = 0, size = 0;
			uint32 next = UINT32_MAX;  // the next draw with the same key in this frame
			uint32 slots = UINT32_MAX; // uniform buffers: first of LATTE_NUM_MAX_UNIFORM_BUFFERS entries in Frame::slots
			uint8 mode = 0;
			bool hasSig = false;
			float sig[kSigWords];
		};

		struct Slot
		{
			uint32 offset = 0, size = 0; // relative to Record::offset; size 0 = not used
		};

		// the draws of a key in a frame
		struct Chain
		{
			uint32 first = UINT32_MAX, last = UINT32_MAX, count = 0;
			sint32 lookup = -1;               // in Frame::lookups, made when the next frame looks for one of several
			const KeyWords* words = nullptr;  // (learned)
		};

		struct Lookup
		{
			std::vector<uint32> records; // in order
		};

		struct Frame
		{
			std::vector<uint8> data;
			std::vector<Record> records;
			std::vector<Slot> slots;
			robin_hood::unordered_flat_map<Key, Chain, KeyHash> byKey;
			std::vector<Lookup> lookups; // (kept allocated, lookupCount in use)
			uint32 lookupCount = 0;

			void Clear()
			{
				data.clear();
				records.clear();
				slots.clear();
				byKey.clear();
				lookupCount = 0;
			}
		};

		Frame s_frames[2];
		uint32 s_cur = 0;
		const uint8* s_ring = nullptr;  // the uniform ring buffer (host memory)
		uint32 s_pending = UINT32_MAX;  // record of this frame waiting for the ring offset of its uniforms
		Key s_draw;
		std::vector<uint8> s_upload;
		uint64 s_frameCounter = 1;

		struct
		{
			uint32 draws = 0, matched = 0, chosen = 0, searched = 0, unmatched = 0, bytes = 0;
			uint64 ticks = 0; // time spent here (dev stat)
		} s_stats, s_lastStats;

		struct
		{
			VKRObjectTexture* tex = nullptr;
			VKRObjectTextureView* view = nullptr;
			uint32 width = 0, height = 0;
			bool initialized = false;
			uint64 clearedFrame = 0;
		} s_velocity;

		const uint32* RecordWords(const Frame& f, const Record& r)
		{
			return (const uint32*)((r.mode == LATTE_DECOMPILER_UNIFORM_MODE_REMAPPED ? s_ring : f.data.data()) + r.offset);
		}

		const Lookup& GetLookup(Frame& f, Chain& chain)
		{
			if (chain.lookup >= 0)
				return f.lookups[chain.lookup];
			if (f.lookupCount == f.lookups.size())
				f.lookups.emplace_back();
			chain.lookup = (sint32)f.lookupCount++;
			Lookup& lk = f.lookups[chain.lookup];
			lk.records.clear();
			for (uint32 i = chain.first; i != UINT32_MAX; i = f.records[i].next)
				lk.records.push_back(i);
			return lk;
		}

		// the words in which the draws of a key differ: the first draw compared with three others
		const KeyWords* Learn(const Frame& f, const Key& key, const Lookup& lk)
		{
			KeyWords kw;
			std::vector<uint32> usable;
			for (uint32 index : lk.records)
				if (f.records[index].offset != UINT32_MAX)
					usable.push_back(index);
			if (usable.size() >= 2)
			{
				const size_t k = usable.size();
				uint32 count = UINT32_MAX;
				for (uint32 index : usable)
					count = std::min(count, f.records[index].size / 4);
				const uint32* first = RecordWords(f, f.records[usable[0]]);
				const uint32* others[3];
				uint32 otherCount = 0;
				for (size_t i : {(size_t)1, k / 2, k - 1})
				{
					if (i == 0)
						continue;
					const uint32* o = RecordWords(f, f.records[usable[i]]);
					if (otherCount == 0 || others[otherCount - 1] != o)
						others[otherCount++] = o;
				}
				for (uint32 w = 0; w < count && kw.count < kSigWords; w++)
				{
					for (uint32 i = 0; i < otherCount; i++)
					{
						if (others[i][w] != first[w])
						{
							kw.word[kw.count++] = (uint16)std::min<uint32>(w, 0xFFFF);
							break;
						}
					}
				}
			}
			return &(s_keyWords[key] = kw);
		}

		// (at the end of a frame: no draw refers to them then)
		void TrimKeyWords()
		{
			if (s_keyWords.size() > 100000)
				s_keyWords.clear();
		}

		float Diff(uint32 a, uint32 b)
		{
			const float x = std::bit_cast<float>(a) - std::bit_cast<float>(b);
			if (std::isfinite(x))
				return std::fabs(x);
			return a != b ? 1e6f : 0.0f;
		}

		VkImageLayout AttachmentLayout()
		{
			return VulkanRenderer::GetInstance()->UseAttachmentFeedbackLoop() ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_GENERAL;
		}

		void ReleaseVelocity()
		{
			auto* vkr = VulkanRenderer::GetInstance();
			if (s_velocity.view)
				vkr->ReleaseDestructibleObject(s_velocity.view);
			if (s_velocity.tex)
				vkr->ReleaseDestructibleObject(s_velocity.tex);
			s_velocity.view = nullptr;
			s_velocity.tex = nullptr;
			s_velocity.width = s_velocity.height = 0;
			s_velocity.initialized = false;
		}
	}

	void SetDraw(uint32 baseVertex, uint32 instanceCount, uint32 count, MPTR indexData)
	{
		s_draw.baseVertex = baseVertex;
		s_draw.instances = instanceCount;
		s_draw.count = count;
		s_draw.indices = indexData;
	}

	bool FillUniforms(LatteDecompilerShader* vs, uint8* uniformBuf, bool velocityPass, const uint8* ring, const std::function<uint32(std::span<uint8>)>& upload)
	{
		s_ring = ring;
		s_pending = UINT32_MAX;
		sint32* pce = (sint32*)(uniformBuf + vs->uniform.loc_pce);
		const uint32 pceInts = vs->pceMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK ? 20 : 4;
		auto none = [&]() {
			bool changed = false;
			for (uint32 i = 0; i < pceInts; i++)
				changed |= pce[i] != 0;
			memset(pce, 0, pceInts * 4);
			return changed;
		};
		if (!velocityPass || g_devMode == 2)
			return none();
		s_stats.draws++;
		struct Timer
		{
			uint64 t0 = HighResolutionTimer::now().getTick();
			~Timer() { s_stats.ticks += HighResolutionTimer::now().getTick() - t0; }
		} timer;
		// this draw and its uniforms
		Key key = s_draw;
		key.vs = vs->baseHash;
		auto* ps = LatteSHRC_GetActivePixelShader();
		key.ps = ps ? ps->baseHash : 0;
		key.vertices = 0;
		if (auto* fetch = LatteSHRC_GetActiveFetchShader(); fetch && !fetch->bufferGroups.empty())
			key.vertices = LatteGPUState.contextRegister[mmSQ_VTX_ATTRIBUTE_BLOCK_START + fetch->bufferGroups[0].attributeBufferIndex * 7 + 0];
		Frame& cur = s_frames[s_cur];
		Record rec;
		rec.mode = vs->pceMode;
		const uint32* curWords;
		if (vs->pceMode == LATTE_DECOMPILER_UNIFORM_MODE_REMAPPED)
		{
			// (the ring offset comes when the uniform var block is uploaded: OnVertexUniforms)
			rec.offset = UINT32_MAX;
			rec.size = vs->pceRemappedCount * 16;
			curWords = (const uint32*)(uniformBuf + vs->uniform.loc_remapped);
		}
		else
		{
			rec.offset = (uint32)cur.data.size();
			rec.slots = (uint32)cur.slots.size();
			cur.slots.resize(cur.slots.size() + LATTE_NUM_MAX_UNIFORM_BUFFERS);
			for (const auto& buf : vs->list_quickBufferList)
			{
				const MPTR addr = LatteGPUState.contextRegister[mmSQ_VTX_UNIFORM_BLOCK_START + buf.index * 7 + 0];
				uint32 size = std::min<uint32>(LatteGPUState.contextRegister[mmSQ_VTX_UNIFORM_BLOCK_START + buf.index * 7 + 1] + 1, buf.size);
				size = addr ? (size + 15) & ~15u : 0;
				Slot& slot = cur.slots[rec.slots + buf.index];
				slot.offset = (uint32)cur.data.size() - rec.offset;
				slot.size = size;
				if (size)
					cur.data.insert(cur.data.end(), memory_base + addr, memory_base + addr + size); // uniform blocks are little endian
			}
			rec.size = (uint32)cur.data.size() - rec.offset;
			curWords = (const uint32*)(cur.data.data() + rec.offset);
		}
		const uint32 curCount = rec.size / 4;
		Chain& chain = cur.byKey[key];
		if (chain.count == 0)
		{
			auto known = s_keyWords.find(key);
			chain.words = known != s_keyWords.end() ? &known->second : nullptr;
		}
		auto sign = [&](Record& r, const KeyWords* kw) {
			for (uint32 i = 0; i < kw->count; i++)
				r.sig[i] = kw->word[i] < curCount ? std::bit_cast<float>(curWords[kw->word[i]]) : 0.0f;
			r.hasSig = true;
		};
		if (chain.words)
			sign(rec, chain.words);
		const uint32 recIndex = (uint32)cur.records.size();
		cur.records.push_back(rec);
		if (chain.last != UINT32_MAX)
			cur.records[chain.last].next = recIndex;
		else
			chain.first = recIndex;
		chain.last = recIndex;
		const uint32 occurrence = chain.count++;
		if (rec.mode == LATTE_DECOMPILER_UNIFORM_MODE_REMAPPED)
			s_pending = recIndex;
		if (g_devMode == 4)
			return none(); // (dev: recorded, not matched)
		// the same draw in the previous frame (several: the one with the nearest uniforms; the same place in the order
		// when it is clearly the nearest of its neighbours)
		Frame& prev = s_frames[s_cur ^ 1];
		auto it = prev.byKey.find(key);
		if (it == prev.byKey.end())
		{
			s_stats.unmatched++;
			return none();
		}
		const Record* match = nullptr;
		if (it->second.count == 1)
			match = &prev.records[it->second.first];
		else
		{
			const Lookup& lk = GetLookup(prev, it->second);
			const KeyWords* kw = chain.words;
			if (!kw)
			{
				kw = Learn(prev, key, lk);
				chain.words = kw;
				sign(cur.records[recIndex], kw);
			}
			const Record& self = cur.records[recIndex];
			const uint32 k = (uint32)lk.records.size();
			auto distance = [&](uint32 i) {
				const Record& r = prev.records[lk.records[i]];
				if (r.offset == UINT32_MAX)
					return FLT_MAX;
				float d = 0.0f;
				if (r.hasSig)
				{
					for (uint32 j = 0; j < kw->count; j++)
						d += Diff(std::bit_cast<uint32>(self.sig[j]), std::bit_cast<uint32>(r.sig[j]));
					return d;
				}
				const uint32* p = RecordWords(prev, r);
				const uint32 pCount = r.size / 4;
				for (uint32 j = 0; j < kw->count; j++)
				{
					const uint32 w = kw->word[j];
					d += (w < curCount && w < pCount) ? Diff(curWords[w], p[w]) : 1e6f;
				}
				return d;
			};
			const uint32 same = std::min(occurrence, k - 1);
			uint32 bestIndex = same;
			float best = distance(same);
			bool sure = best == 0.0f;
			if (!sure)
			{
				float neighbour = FLT_MAX;
				if (same > 0)
					neighbour = std::min(neighbour, distance(same - 1));
				if (same + 1 < k)
					neighbour = std::min(neighbour, distance(same + 1));
				sure = best * 4.0f < neighbour;
			}
			if (!sure)
			{
				s_stats.searched++;
				for (uint32 i = 0; i < k; i++)
				{
					const float d = distance(i);
					if (d < best)
					{
						best = d;
						bestIndex = i;
					}
				}
			}
			match = &prev.records[lk.records[bestIndex]];
			s_stats.chosen++;
		}
		if (match->mode != rec.mode || match->offset == UINT32_MAX || (rec.mode == LATTE_DECOMPILER_UNIFORM_MODE_REMAPPED && match->size != rec.size))
		{
			s_stats.unmatched++;
			return none();
		}
		s_stats.matched++;
		if (g_devMode == 1)
			return none();
		// its uniforms for the shader
		memset(pce, 0, pceInts * 4);
		pce[0] = 1;
		if (rec.mode == LATTE_DECOMPILER_UNIFORM_MODE_REMAPPED)
		{
			pce[1] = (sint32)(match->offset / 16); // where they still are in the uniform ring buffer
		}
		else
		{
			// uniform buffers one after the other (the ones the previous draw did not have: the current ones)
			s_upload.clear();
			uint32 rel[LATTE_NUM_MAX_UNIFORM_BUFFERS]{};
			for (const auto& buf : vs->list_quickBufferList)
			{
				const uint32 i = buf.index;
				rel[i] = (uint32)s_upload.size();
				const Slot& mine = cur.slots[rec.slots + i];
				const Slot& old = prev.slots[match->slots + i];
				const bool fromPrev = old.size != 0;
				const uint8* src = fromPrev ? prev.data.data() + match->offset + old.offset : cur.data.data() + rec.offset + mine.offset;
				const uint32 size = fromPrev ? old.size : mine.size;
				s_upload.insert(s_upload.end(), src, src + size);
			}
			s_upload.resize(s_upload.size() + 256, 0); // (reads past a block stay inside)
			const uint32 offset = upload(std::span<uint8>(s_upload.data(), s_upload.size()));
			for (const auto& buf : vs->list_quickBufferList)
				pce[4 + buf.index] = (sint32)((offset + rel[buf.index]) / 16);
			s_stats.bytes += (uint32)s_upload.size();
		}
		if (g_devMode == 3)
			pce[0] = 0; // (dev: prepared, but the shaders skip it)
		return true;
	}

	void OnVertexUniforms(uint32 remappedRingOffset)
	{
		if (s_pending == UINT32_MAX)
			return;
		Frame& cur = s_frames[s_cur];
		if (s_pending < cur.records.size())
			cur.records[s_pending].offset = remappedRingOffset;
		s_pending = UINT32_MAX;
	}

	void OnFrameEnd()
	{
		VulkanRenderer::GetInstance()->PCE_OnFrameEnd();
		s_pending = UINT32_MAX;
		TrimKeyWords();
		s_cur ^= 1;
		s_frames[s_cur].Clear();
		s_lastStats = s_stats;
		s_stats = {};
		s_frameCounter++;
	}

	VKRObjectTextureView* GetVelocityView(uint32 width, uint32 height)
	{
		if (s_velocity.view && s_velocity.width == width && s_velocity.height == height)
			return s_velocity.view;
		ReleaseVelocity();
		auto* vkr = VulkanRenderer::GetInstance();
		const VkDevice device = vkr->GetLogicalDevice();
		VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = kVelocityFormat;
		ii.extent = {width, height, 1};
		ii.mipLevels = 1;
		ii.arrayLayers = 1;
		ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = VK_IMAGE_TILING_OPTIMAL;
		ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		if (vkr->UseAttachmentFeedbackLoop())
			ii.usage |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
		ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		auto* tex = new VKRObjectTexture();
		if (vkCreateImage(device, &ii, nullptr, &tex->m_image) != VK_SUCCESS)
		{
			delete tex;
			cemuLog_log(LogType::Force, "Frame generation: velocity image not created");
			return nullptr;
		}
		tex->m_format = kVelocityFormat;
		tex->m_imageAspect = VK_IMAGE_ASPECT_COLOR_BIT;
		tex->m_flags = 0;
		tex->m_allocation = vkr->GetMemoryManager()->imageMemoryAllocate(tex->m_image);
		VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image = tex->m_image;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = kVelocityFormat;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkImageView view;
		if (vkCreateImageView(device, &vi, nullptr, &view) != VK_SUCCESS)
		{
			vkr->ReleaseDestructibleObject(tex);
			return nullptr;
		}
		s_velocity.tex = tex;
		s_velocity.view = new VKRObjectTextureView(tex, view);
		s_velocity.width = width;
		s_velocity.height = height;
		s_velocity.initialized = false;
		s_velocity.clearedFrame = 0;
		return s_velocity.view;
	}

	void BeginVelocityPass(VkCommandBuffer cmd, VKRObjectTextureView* view)
	{
		if (!view || view != s_velocity.view || (s_velocity.initialized && s_velocity.clearedFrame == s_frameCounter))
			return;
		const VkImageLayout attachment = AttachmentLayout();
		VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
		b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.oldLayout = s_velocity.initialized ? attachment : VK_IMAGE_LAYOUT_UNDEFINED;
		b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = s_velocity.tex->m_image;
		b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
			nullptr, 0, nullptr, 1, &b);
		VkClearColorValue clear{};
		clear.float32[0] = clear.float32[1] = kNoVelocity;
		vkCmdClearColorImage(cmd, s_velocity.tex->m_image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &b.subresourceRange);
		b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
		b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		b.newLayout = attachment;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
		s_velocity.tex->flagForCurrentCommandBuffer();
		s_velocity.view->flagForCurrentCommandBuffer();
		s_velocity.initialized = true;
		s_velocity.clearedFrame = s_frameCounter;
	}

	bool GetVelocityForRead(VelocityImage& out)
	{
		if (!s_velocity.view || !s_velocity.initialized || s_velocity.clearedFrame != s_frameCounter)
			return false;
		out.image = s_velocity.tex->m_image;
		out.view = s_velocity.view->m_textureImageView;
		out.layout = AttachmentLayout();
		out.width = s_velocity.width;
		out.height = s_velocity.height;
		s_velocity.tex->flagForCurrentCommandBuffer();
		s_velocity.view->flagForCurrentCommandBuffer();
		return true;
	}

	std::string Stats()
	{
		return fmt::format("draws {} matched {} (of several {}, searched {}) unmatched {} | uploaded {} KB, {:.2f} ms | velocity {}x{} | async pipelines {} (velocity {})",
			s_lastStats.draws, s_lastStats.matched, s_lastStats.chosen, s_lastStats.searched, s_lastStats.unmatched, s_lastStats.bytes / 1024,
			(double)s_lastStats.ticks * 1000.0 / (double)HighResolutionTimer::getFrequency(), s_velocity.width, s_velocity.height, g_asyncPipelines.load(),
			g_asyncVelocityPipelines.load());
	}
}

// (LatteRenderTarget.cpp, at the end of a frame)
void LattePce_OnFrameEnd()
{
	VulkanPce::OnFrameEnd();
}

std::string LattePce_Stats()
{
	return VulkanPce::Stats();
}

void LattePce_SetDevMode(int mode)
{
	VulkanPce::g_devMode = mode;
}
