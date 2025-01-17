#include "svg/SvgCache.h"
#include "svg/Svg.h"
#include "animation/AnimationManager.h"
#include "animation/Animation.h"
#include "renderer/Texture.h"
#include "renderer/Renderer.h"
#include "renderer/GLApi.h"
#include "math/CMath.h"
#include "core/Profiling.h"
#include "editor/panels/ExportPanel.h"

namespace MathAnim
{
	Vec2 SvgCache::cachePadding = { 10.0f, 10.0f };

	void SvgCache::init()
	{
		constexpr int defaultWidth = 4096;
		this->generateDefaultFramebuffer(defaultWidth, defaultWidth);
	}

	void SvgCache::free()
	{
		framebuffer.destroy();
		cachedSvgs.clear();
	}

	bool SvgCache::exists(AnimationManagerData* am, AnimObjId obj)
	{
		const AnimObject* animObj = AnimationManager::getObject(am, obj);
		if (!animObj->svgObject || !animObj->svgObject->md5) return false;

		return existsInternal(hash(animObj->svgObject->md5, animObj->svgObject->md5Length, animObj->svgScale, animObj->percentReplacementTransformed));
	}

	SvgCacheEntry SvgCache::get(AnimationManagerData* am, AnimObjId obj)
	{
		const AnimObject* animObj = AnimationManager::getObject(am, obj);
		if (animObj->svgObject && animObj->svgObject->md5)
		{
			auto entry = getInternal(hash(animObj->svgObject->md5, animObj->svgObject->md5Length, animObj->svgScale, animObj->percentReplacementTransformed));
			if (entry.has_value())
			{
				return SvgCacheEntry{
					entry->texCoordsMin,
					entry->texCoordsMax,
					framebuffer.getColorAttachment(entry->colorAttachment)
				};
			}
		}

		static Texture dummy = TextureBuilder()
			.setWidth(1)
			.setHeight(1)
			.setFormat(ByteFormat::RGB8_UI)
			.setMagFilter(FilterMode::Linear)
			.setMinFilter(FilterMode::Linear)
			.generate();
		return SvgCacheEntry{ Vec2{0, 0}, Vec2{1, 1}, dummy };
	}

	SvgCacheEntry SvgCache::getOrCreateIfNotExist(AnimationManagerData* am, SvgObject* svg, AnimObjId obj)
	{
		MP_PROFILE_EVENT("SvgCache_GetOrCreateIfNotExists");
		const AnimObject* animObj = AnimationManager::getObject(am, obj);
		if (animObj->svgObject && animObj->svgObject->md5)
		{
			auto entry = getInternal(hash(animObj->svgObject->md5, animObj->svgObject->md5Length, animObj->svgScale, animObj->percentReplacementTransformed));
			if (entry.has_value())
			{
				return SvgCacheEntry{
					entry->texCoordsMin,
					entry->texCoordsMax,
					framebuffer.getColorAttachment(entry->colorAttachment)
				};
			}
		}

		put(animObj, svg);
		return get(am, obj);
	}

	void SvgCache::put(const AnimObject* parent, SvgObject* svg)
	{
		MP_PROFILE_EVENT("SvgCache_Put");
		uint64 hashValue = hash(svg->md5, svg->md5Length, parent->svgScale, parent->percentReplacementTransformed);

		// Only add the SVG if it hasn't already been added
		if (!existsInternal(hashValue))
		{
			// Setup the texture coords and everything 
			Vec2 svgTextureOffset = cacheCurrentPos;
			int colorAttachmentToRenderTo = this->cacheCurrentColorAttachment;

			// Check if the SVG cache needs to regenerate
			float svgTotalWidth = ((svg->bbox.max.x - svg->bbox.min.x) * parent->svgScale);
			float svgTotalHeight = ((svg->bbox.max.y - svg->bbox.min.y) * parent->svgScale);
			if (svgTotalWidth <= 0.0f || svgTotalHeight <= 0.0f)
			{
				return;
			}

			bool incrementX = true;
			Vec2 allottedSize = Vec2{ svgTotalWidth, svgTotalHeight };
			{
				int newRightX = (int)(svgTextureOffset.x + svgTotalWidth + cachePadding.x);
				if (newRightX >= framebuffer.width)
				{
					// Move to the newline
					svgTextureOffset = incrementCacheCurrentY();
				}

				float newBottomY = svgTextureOffset.y + svgTotalHeight + cachePadding.y;
				if (newBottomY >= framebuffer.height)
				{
					// Evict the first potential result from the LRU cache that
					// can contain the size of this SVG

					// Only evict up to the oldest evictionThreshold% entries. If nothing is big enough in the oldest 
					// evictionThreshold% entries then grow the cache instead of evicting a newer entry
					constexpr float evictionThreshold = 0.1f;
					LRUCacheEntry<uint64, _SvgCacheEntryInternal>* oldest = this->cachedSvgs.getOldest();
					const size_t maxNumEntriesToTry = (size_t)(evictionThreshold * (float)this->cachedSvgs.size());
					size_t counter = 0;
					while (oldest != nullptr && counter < maxNumEntriesToTry)
					{
						if (oldest->data.allottedSize.x >= svgTotalWidth && oldest->data.allottedSize.y >= svgTotalHeight)
						{
							// We found an entry that will fit this 
							// in the future we could evict this entry, 
							// repack the texture and then insert the new svg
							// but in practice this will probably be too slow

							// So for now I'll just "evict" then re-insert the new entry

							allottedSize = oldest->data.allottedSize;
							svgTextureOffset = oldest->data.textureOffset;
							colorAttachmentToRenderTo = oldest->data.colorAttachment;

							if (!this->cachedSvgs.evict(oldest->key))
							{
								g_logger_error("SVG cache eviction failed: 0x%8x", oldest->key);
								oldest = nullptr;
							}

							// The svg will get reinserted below
							break;
						}

						// Try to find an entry that will fit this new SVG
						oldest = oldest->next;
						counter++;
					}

					if (oldest == nullptr || counter >= maxNumEntriesToTry)
					{
						// Didn't find room so just clear an entire texture and let
						// everything get recached
						growCache();
						svgTextureOffset = cacheCurrentPos;
						colorAttachmentToRenderTo = this->cacheCurrentColorAttachment;
					}
					else
					{
						const Texture& textureToRenderTo = framebuffer.getColorAttachment(colorAttachmentToRenderTo);
						GL::enable(GL_SCISSOR_TEST);
						GL::scissor(
							(GLint)svgTextureOffset.x,
							(GLint)(textureToRenderTo.height - svgTextureOffset.y - allottedSize.y),
							(GLsizei)allottedSize.x,
							(GLsizei)allottedSize.y
						);
						framebuffer.bind();
						framebuffer.clearColorAttachmentRgba(colorAttachmentToRenderTo, Vec4{ 0.0f, 0, 0, 0.0f });
						framebuffer.clearDepthStencil();
						incrementX = false;
						GL::disable(GL_SCISSOR_TEST);
					}
				}
			}

			// Calculate UVs and stuff for LRU cache
			Vec2 cacheUvMin = Vec2{
				svgTextureOffset.x / framebuffer.width,
				1.0f - (svgTextureOffset.y / framebuffer.width) - (svgTotalHeight / framebuffer.height)
			};
			Vec2 cacheUvMax = cacheUvMin +
				Vec2{
					svgTotalWidth / framebuffer.width,
					svgTotalHeight / framebuffer.height
			};

			if (incrementX)
			{
				incrementCacheCurrentX(svgTotalWidth + cachePadding.x);
				checkLineHeight(svgTotalHeight);
			}

			// Store the results here
			_SvgCacheEntryInternal res = {};
			res.colorAttachment = colorAttachmentToRenderTo;
			res.texCoordsMin = cacheUvMin;
			res.texCoordsMax = cacheUvMax;
			res.svgSize = Vec2{ svgTotalWidth, svgTotalHeight };
			res.allottedSize = allottedSize;
			res.textureOffset = svgTextureOffset;
			this->cachedSvgs.insert(hashValue, res);

			// Then begin the rasterization after we've updated the LRU cache

			// If we're exporting video, frame drops don't matter and we want every frame
			// exported to the encoder to be perfect
			if (ExportPanel::isExportingVideo())
			{
				svg->render(
					parent->svgScale,
					framebuffer.getColorAttachment(colorAttachmentToRenderTo),
					svgTextureOffset
				);
			}
			else
			{
				// Otherwise, it's ok if we don't get the texture immediately,
				// so we can dump it on a background thread and wait for the result
				svg->renderAsync(
					parent->svgScale,
					framebuffer.getColorAttachment(colorAttachmentToRenderTo),
					svgTextureOffset
				);
			}
		}
	}

	void SvgCache::render(AnimationManagerData* am, SvgObject* svg, AnimObjId obj)
	{
		MP_PROFILE_EVENT("SvgCache_Render");

		const AnimObject* parent = AnimationManager::getObject(am, obj);
		if (parent)
		{
			SvgCacheEntry metadata = getOrCreateIfNotExist(am, svg, obj);
			// TODO: See if I can get rid of this duplication, see the function above
			float svgTotalWidth = ((svg->bbox.max.x - svg->bbox.min.x) * parent->svgScale);
			float svgTotalHeight = ((svg->bbox.max.y - svg->bbox.min.y) * parent->svgScale);

			if (parent->is3D)
			{
				Renderer::drawTexturedQuad3D(
					metadata.textureRef,
					Vec2{ svgTotalWidth / parent->svgScale, svgTotalHeight / parent->svgScale },
					metadata.texCoordsMin,
					metadata.texCoordsMax,
					parent->globalTransform,
					parent->isTransparent
				);
			}
			else
			{
				Renderer::drawTexturedQuad(
					metadata.textureRef,
					Vec2{ svgTotalWidth / parent->svgScale, svgTotalHeight / parent->svgScale },
					metadata.texCoordsMin,
					metadata.texCoordsMax,
					Vec4{
						(float)parent->fillColor.r / 255.0f,
						(float)parent->fillColor.g / 255.0f,
						(float)parent->fillColor.b / 255.0f,
						(float)parent->fillColor.a / 255.0f
					},
					parent->id,
					parent->globalTransform
				);
			}
		}
	}

	const Framebuffer& SvgCache::getFramebuffer()
	{
		return framebuffer;
	}

	void SvgCache::clearAll()
	{
		cacheCurrentPos.x = 0;
		cacheCurrentPos.y = 0;
		cacheCurrentColorAttachment = 0;
		cacheLineHeight = 0;
		cachedSvgs.clear();

		GL::pushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, "SVG_Cache_Reset");

		framebuffer.bind();
		GL::viewport(0, 0, framebuffer.width, framebuffer.height);
		for (int i = 0; i < framebuffer.colorAttachments.size(); i++)
		{
			framebuffer.clearColorAttachmentRgba(i, Vec4{ 0, 0, 0, 0 });
		}
		framebuffer.clearDepthStencil();

		GL::popDebugGroup();
	}

	// --------------------- Private ---------------------
	Vec2 SvgCache::incrementCacheCurrentY()
	{
		cacheCurrentPos.y += cacheLineHeight + cachePadding.y;
		cacheLineHeight = 0;
		cacheCurrentPos.x = 0;
		return cacheCurrentPos;
	}

	Vec2 SvgCache::incrementCacheCurrentX(float distance)
	{
		cacheCurrentPos.x += distance;
		return cacheCurrentPos;
	}

	void SvgCache::checkLineHeight(float newLineHeight)
	{
		cacheLineHeight = glm::max(cacheLineHeight, newLineHeight);
	}

	void SvgCache::growCache()
	{
		// TODO: This should just add a new color attachment
		cacheCurrentColorAttachment = (cacheCurrentColorAttachment + 1) % framebuffer.colorAttachments.size();
		{
			// Delete all cached svg entries that are attached to cacheCurrentColorAttachment
			LRUCacheEntry<uint64, _SvgCacheEntryInternal>* oldest = this->cachedSvgs.getOldest();
			while (oldest != nullptr)
			{
				if (oldest->data.colorAttachment == cacheCurrentColorAttachment)
				{
					LRUCacheEntry<uint64, _SvgCacheEntryInternal>* next = oldest->next;
					if (!cachedSvgs.evict(oldest->key))
					{
						g_logger_error("Failed to evict cache entry %u", oldest->key);
					}
					oldest = next;
				}
				else
				{
					oldest = oldest->next;
				}
			}
		}

		// Clear the color attachment
		framebuffer.bind();
		GL::viewport(0, 0, framebuffer.width, framebuffer.height);
		this->framebuffer.clearColorAttachmentRgba(cacheCurrentColorAttachment, Vec4{ 0, 0, 0, 0 });
		framebuffer.clearDepthStencil();
		this->cacheCurrentPos = Vec2{ 0, 0 };
	}

	std::optional<_SvgCacheEntryInternal> SvgCache::getInternal(uint64 hash)
	{
		const auto& res = cachedSvgs.get(hash);
		if (res.has_value())
		{
			return res;
		}

		return std::nullopt;
	}

	bool SvgCache::existsInternal(uint64 hash)
	{
		return cachedSvgs.exists(hash);
	}

	void SvgCache::generateDefaultFramebuffer(uint32 width, uint32 height)
	{
		if (width > 4096 || height > 4096)
		{
			g_logger_error("SVG cache cannot be bigger than 4096x4096 pixels. The SVG will be truncated.");
			width = 4096;
			height = 4096;
		}

		// Default the svg framebuffer cache to 1024x1024 and resize if necessary
		Texture cacheTexture = TextureBuilder()
			.setFormat(ByteFormat::RGBA8_UI)
			.setMinFilter(FilterMode::Linear)
			.setMagFilter(FilterMode::Linear)
			.setWidth(width)
			.setHeight(height)
			.build();
		// Add four cached textures and increment through them
		// for more space when "growing" the svg cache
		framebuffer = FramebufferBuilder(width, height)
			.addColorAttachment(cacheTexture)
			.addColorAttachment(cacheTexture)
			.addColorAttachment(cacheTexture)
			.addColorAttachment(cacheTexture)
			.includeDepthStencil()
			.generate();
		cachedSvgs = {};
	}

	uint64 SvgCache::hash(const uint8* svgMd5, size_t svgMd5Length, float svgScale, float replacementTransform)
	{
		uint64 hash = 0;
		// Only hash floating point numbers to 3 decimal places
		int roundedSvgScale = (int)(svgScale * 1000.0f);
		hash = CMath::combineHash<int>(roundedSvgScale, hash);
		int roundedTransform = (int)(replacementTransform * 100.0f);
		hash = CMath::combineHash<int>(roundedTransform, hash);
		uint64 md5Hash = std::hash<std::string>{}(std::string((const char*)svgMd5, svgMd5Length));
		hash = CMath::combineHash<uint64>(md5Hash, hash);
		return hash;
	}
}
