#include "editor/Gizmos.h"
#include "editor/EditorGui.h"
#include "animation/AnimationManager.h"
#include "renderer/Framebuffer.h"
#include "renderer/Renderer.h"
#include "renderer/OrthoCamera.h"
#include "renderer/PerspectiveCamera.h"
#include "renderer/Colors.h"
#include "core/Input.h"
#include "core/Application.h"
#include "core/Profiling.h"
#include "math/CMath.h"

namespace MathAnim
{
	// -------------- Internal Structures --------------
	enum class FollowMouseMoveMode : uint8
	{
		None = 0,
		HzOnly,
		VtOnly,
		FreeMove
	};

	struct GizmoState
	{
		GizmoType gizmoType;
		GizmoVariant variant;
		uint64 idHash;
		Vec3 position;
		Vec3 positionMoveStart;
		Vec3 mouseDelta;
		FollowMouseMoveMode moveMode;
		bool shouldDraw;

		void render();
	};

	struct GlobalContext
	{
		// Maps from idHash to index in gizmos vector
		std::unordered_map<uint64, uint32> gizmoById;
		std::vector<GizmoState*> gizmos;

		uint64 hoveredGizmo;
		uint64 activeGizmo;
		uint64 lastActiveGizmo; // Gizmo active last frame
		GizmoVariant hotGizmoVariant; // Variant of the above IDs
		Vec3 mouseWorldPos3f;
		Vec2 mouseWorldPos2f;
	};

	// -------------- Internal Functions --------------
	static inline Vec2 getGizmoPos(const Vec3& position, const Vec2& offset, float cameraZoom)
	{
		return CMath::vector2From3(position) + offset * cameraZoom;
	}

	namespace GizmoManager
	{
		// -------------- Internal Variables --------------
		static GlobalContext* gGizmoManager;
		static constexpr Vec2 defaultFreeMoveSize = Vec2{ 0.45f, 0.45f };
		static constexpr Vec2 defaultVerticalMoveSize = Vec2{ 0.08f, 0.6f };
		static constexpr Vec2 defaultHorizontalMoveSize = Vec2{ 0.6f, 0.08f };

		static constexpr Vec2 defaultVerticalMoveOffset = Vec2{ -0.4f, 0.1f };
		static constexpr Vec2 defaultHorizontalMoveOffset = Vec2{ 0.1f, -0.4f };
		static constexpr float defaultArrowTipHeight = 0.25f;
		static constexpr float defaultArrowTipHalfWidth = 0.1f;

		static constexpr uint64 NullGizmo = UINT64_MAX;

		// -------------- Internal Functions --------------
		static GizmoState* getGizmoByName(const char* name);
		static GizmoState* getGizmoById(uint64 id);
		static uint64 hashName(const char* name);
		static GizmoState* createDefaultGizmoState(const char* name, GizmoType type);
		static bool isMouseHovered(const Vec2& centerPosition, const Vec2& size);
		static Vec3 getMouseWorldPos3f();
		static Vec2 getMouseWorldPos2f();
		static void handleActiveCheck(GizmoState* gizmo, const Vec2& offset, const Vec2& gizmoSize, float cameraZoom);

		void init()
		{
			void* gMemory = g_memory_allocate(sizeof(GlobalContext));
			gGizmoManager = new(gMemory)GlobalContext();
			gGizmoManager->hoveredGizmo = NullGizmo;
			gGizmoManager->activeGizmo = NullGizmo;
			gGizmoManager->mouseWorldPos3f = Vec3{ NAN, NAN, NAN };
			gGizmoManager->mouseWorldPos2f = Vec2{ 0.0f, 0.0f };
		}

		void update(AnimationManagerData* am)
		{
			MP_PROFILE_EVENT("Gizmo_Update");
			GlobalContext* g = gGizmoManager;
			g->lastActiveGizmo = g->activeGizmo;
			//g->mouseWorldPos3f = getMouseWorldPos3f();
			g->mouseWorldPos2f = getMouseWorldPos2f();

			EditorGui::onGizmo(am);
		}

		void render(AnimationManagerData* am)
		{
			MP_PROFILE_EVENT("Gizmo_Render");

			// Render call stuff
			GlobalContext* g = gGizmoManager;
			for (auto iter : g->gizmos)
			{
				if (iter->shouldDraw)
				{
					// Draw the gizmo
					iter->render();
				}
			}

			// End Frame stuff
			for (auto iter : g->gizmos)
			{
				iter->shouldDraw = false;
			}

			const AnimObject* orthoCameraObj = AnimationManager::getActiveOrthoCamera(am);
			if (orthoCameraObj)
			{
				if (orthoCameraObj->as.camera.is2D)
				{
					const OrthoCamera& orthoCamera = orthoCameraObj->as.camera.camera2D;
					// Draw camera outlines
					Renderer::pushStrokeWidth(0.05f);
					Renderer::pushColor(Colors::Neutral[0]);
					Renderer::drawSquare(orthoCamera.position - orthoCamera.projectionSize / 2.0f, orthoCamera.projectionSize);
					Renderer::popColor();
					Renderer::popStrokeWidth();
				}
			}
		}

		void free()
		{
			if (gGizmoManager)
			{
				auto iter = gGizmoManager->gizmos.begin();
				while (iter != gGizmoManager->gizmos.end())
				{
					g_memory_free(*iter);
					iter = gGizmoManager->gizmos.erase(iter);
				}

				gGizmoManager->~GlobalContext();
				g_memory_free(gGizmoManager);
				gGizmoManager = nullptr;
			}
		}

		bool anyGizmoActive()
		{
			// Return whether any gizmo was active this frame or last frame 
			// so that it reflects the state of queries accurately
			return gGizmoManager->activeGizmo != NullGizmo || gGizmoManager->lastActiveGizmo != NullGizmo;
		}

		bool translateGizmo(const char* gizmoName, Vec3* position, GizmoVariant variant)
		{
			GlobalContext* g = gGizmoManager;

			// Find or create the gizmo
			GizmoState* gizmo = getGizmoByName(gizmoName);
			if (!gizmo)
			{
				gizmo = createDefaultGizmoState(gizmoName, GizmoType::Translation);
				// Initialize to something sensible
				gizmo->positionMoveStart = *position;
			}
			gizmo->position = *position;
			gizmo->variant = variant;

			if (Input::keyPressed(GLFW_KEY_G))
			{
				switch (gizmo->moveMode)
				{
				case FollowMouseMoveMode::None:
					gizmo->positionMoveStart = *position;
					gizmo->mouseDelta = gizmo->positionMoveStart - CMath::vector3From2(g->mouseWorldPos2f);
					gizmo->moveMode = FollowMouseMoveMode::FreeMove;
					break;
				case FollowMouseMoveMode::HzOnly:
				case FollowMouseMoveMode::VtOnly:
				case FollowMouseMoveMode::FreeMove:
					break;
				}
			}

			if (gizmo->moveMode != FollowMouseMoveMode::None)
			{
				if (Input::keyPressed(GLFW_KEY_X))
				{
					switch (gizmo->moveMode)
					{
					case FollowMouseMoveMode::VtOnly:
					case FollowMouseMoveMode::FreeMove:
						gizmo->moveMode = FollowMouseMoveMode::HzOnly;
						break;
					case FollowMouseMoveMode::HzOnly:
					case FollowMouseMoveMode::None:
						break;
					}
					*position = gizmo->positionMoveStart;
				}
				if (Input::keyPressed(GLFW_KEY_Y))
				{
					switch (gizmo->moveMode)
					{
					case FollowMouseMoveMode::HzOnly:
					case FollowMouseMoveMode::FreeMove:
						gizmo->moveMode = FollowMouseMoveMode::VtOnly;
						break;
					case FollowMouseMoveMode::VtOnly:
					case FollowMouseMoveMode::None:
						break;
					}
					*position = gizmo->positionMoveStart;
				}
				if (Input::keyPressed(GLFW_KEY_ESCAPE))
				{
					// We can return immediately from a cancel operation
					*position = gizmo->positionMoveStart;
					gizmo->moveMode = FollowMouseMoveMode::None;
					return true;
				}
			}

			const OrthoCamera* camera = Application::getEditorCamera();
			if (gizmo->moveMode != FollowMouseMoveMode::None)
			{
				Vec2 mousePos = EditorGui::mouseToNormalizedViewport();
				Vec2 unprojectedMousePos = camera->reverseProject(mousePos);
				switch (gizmo->moveMode)
				{
				case FollowMouseMoveMode::VtOnly:
					position->y = unprojectedMousePos.y + gizmo->mouseDelta.y;
					gizmo->shouldDraw = true;
					break;
				case FollowMouseMoveMode::HzOnly:
					position->x = unprojectedMousePos.x + gizmo->mouseDelta.x;
					gizmo->shouldDraw = true;
					break;
				case FollowMouseMoveMode::FreeMove:
					position->x = unprojectedMousePos.x + gizmo->mouseDelta.x;
					position->y = unprojectedMousePos.y + gizmo->mouseDelta.y;
					break;
				case FollowMouseMoveMode::None:
					break;
				}

				g->activeGizmo = gizmo->idHash;
				if (Input::mouseClicked(MouseButton::Left))
				{
					gizmo->moveMode = FollowMouseMoveMode::None;
					g->activeGizmo = NullGizmo;
				}

				// If we're in gizmo follow mouse mode, then every frame results in a change operation.
				// So, we return true here every time.
				return true;
			}

			// Otherwise, we continue on to the regular gizmo logic

			gizmo->shouldDraw = true;
			if (g->hoveredGizmo == NullGizmo && g->activeGizmo == NullGizmo)
			{
				// Check if free move variant is hovered
				if (isMouseHovered(CMath::vector2From3(gizmo->position), defaultFreeMoveSize * camera->zoom))
				{
					g->hoveredGizmo = gizmo->idHash;
					g->hotGizmoVariant = GizmoVariant::Free;
				}
				else if (isMouseHovered(getGizmoPos(gizmo->position, defaultVerticalMoveOffset, camera->zoom), defaultVerticalMoveSize * camera->zoom))
				{
					g->hoveredGizmo = gizmo->idHash;
					g->hotGizmoVariant = GizmoVariant::Vertical;
				}
				else if (isMouseHovered(getGizmoPos(gizmo->position, defaultHorizontalMoveOffset, camera->zoom), defaultHorizontalMoveSize * camera->zoom))
				{
					g->hoveredGizmo = gizmo->idHash;
					g->hotGizmoVariant = GizmoVariant::Horizontal;
				}
			}

			if (g->hoveredGizmo == gizmo->idHash)
			{
				// Gizmo is "active" if the user is holding the mouse button down on the gizmo
				// Check if free move variant is changed to active
				if (g->hotGizmoVariant == GizmoVariant::Free)
				{
					handleActiveCheck(gizmo, Vec2{ 0, 0 }, defaultFreeMoveSize, camera->zoom);
				}
				// Check if vertical move is changed to active
				else if (g->hotGizmoVariant == GizmoVariant::Vertical)
				{
					handleActiveCheck(gizmo, defaultVerticalMoveOffset, defaultVerticalMoveSize, camera->zoom);
				}
				// Check if horizontal move is changed to active
				if (g->hotGizmoVariant == GizmoVariant::Horizontal)
				{
					handleActiveCheck(gizmo, defaultHorizontalMoveOffset, defaultHorizontalMoveSize, camera->zoom);
				}
			}

			bool modified = false;
			if (g->activeGizmo == gizmo->idHash)
			{
				if (Input::mouseUp(MouseButton::Left))
				{
					g->activeGizmo = NullGizmo;
				}
				else if (Input::keyPressed(GLFW_KEY_ESCAPE))
				{
					// Cancel move operation
					g->activeGizmo = NullGizmo;
					*position = gizmo->positionMoveStart;
					modified = true;
				}
				else
				{
					// Handle mouse dragging
					// Transform the position passed in to the current mouse position
					switch (g->hotGizmoVariant)
					{
					case GizmoVariant::Free:
						*position = CMath::vector3From2(g->mouseWorldPos2f);
						break;
					case GizmoVariant::Vertical:
						// NOTE: We subtract mouseDelta here to make sure it's offset properly from the mouse
						// and then when it gets added back in below it cancels the operation
						*position = Vec3{ position->x - gizmo->mouseDelta.x, g->mouseWorldPos2f.y, 0.0f };
						break;
					case GizmoVariant::Horizontal:
						// NOTE: Same note as above
						*position = Vec3{ g->mouseWorldPos2f.x, position->y - gizmo->mouseDelta.y, 0.0f };
						break;
					case GizmoVariant::None:
					case GizmoVariant::All:
						break;
					}

					// Add back in mouse delta to make sure we maintain original distance
					// when we started dragging the object
					*position += gizmo->mouseDelta;
					gizmo->position = *position;
					modified = true;
				}
			}

			return modified;
		}

		// -------------- Internal Functions --------------
		static GizmoState* getGizmoByName(const char* name)
		{
			uint64 gizmoId = hashName(name);
			return getGizmoById(gizmoId);
		}

		static GizmoState* getGizmoById(uint64 id)
		{
			GlobalContext* g = gGizmoManager;
			auto iter = g->gizmoById.find(id);
			if (iter == g->gizmoById.end())
			{
				return nullptr;
			}

			return g->gizmos[iter->second];
		}

		static uint64 hashName(const char* name)
		{
			constexpr uint64 FNVOffsetBasis = 0xcbf29ce48422232ULL;
			constexpr uint64 FNVPrime = 0x00000100000001B3ULL;

			uint64 hash = FNVOffsetBasis;
			size_t strLen = std::strlen(name);
			for (size_t i = 0; i < strLen; i++)
			{
				hash = hash * FNVPrime;
				hash = hash ^ name[i];
			}

			return hash;
		}

		static GizmoState* createDefaultGizmoState(const char* name, GizmoType type)
		{
			GizmoState* gizmoState = (GizmoState*)g_memory_allocate(sizeof(GizmoState));
			gizmoState->gizmoType = type;
			uint64 hash = hashName(name);
			gizmoState->idHash = hash;
			gizmoState->shouldDraw = false;
			gizmoState->moveMode = FollowMouseMoveMode::None;
			gizmoState->mouseDelta = Vec3{ 0.0f, 0.0f, 0.0f };

			GlobalContext* g = gGizmoManager;
			g->gizmos.push_back(gizmoState);
			g->gizmoById[hash] = (uint32)g->gizmos.size() - 1;

			return gizmoState;
		}

		static bool isMouseHovered(const Vec2& centerPosition, const Vec2& size)
		{
			const Vec2& worldCoords = gGizmoManager->mouseWorldPos2f;
			Vec2 bottomLeft = centerPosition - (Vec2{ size.x, size.y } / 2.0f);

			return worldCoords.x >= bottomLeft.x && worldCoords.x <= bottomLeft.x + size.x &&
				worldCoords.y >= bottomLeft.y && worldCoords.y <= bottomLeft.y + size.y;
		}

		static void handleActiveCheck(GizmoState* gizmo, const Vec2& offset, const Vec2& gizmoSize, float cameraZoom)
		{
			GlobalContext* g = gGizmoManager;
			if (Input::mouseDown(MouseButton::Left) && isMouseHovered(getGizmoPos(gizmo->position, offset, cameraZoom), gizmoSize * cameraZoom))
			{
				gizmo->mouseDelta = gizmo->position - CMath::vector3From2(g->mouseWorldPos2f);
				g->activeGizmo = gizmo->idHash;
				g->hoveredGizmo = NullGizmo;
			}
			else if (!isMouseHovered(getGizmoPos(gizmo->position, offset, cameraZoom), gizmoSize * cameraZoom))
			{
				g->hoveredGizmo = NullGizmo;
			}
		}

#pragma warning( push )
#pragma warning( disable : 4505 )
		static Vec3 getMouseWorldPos3f()
		{
			g_logger_warning("TODO: Implement 3D getMouseWorldPos3f() in gizmos.cpp");
			return Vec3{ NAN, NAN, NAN };
		}
#pragma warning( pop )

		static Vec2 getMouseWorldPos2f()
		{
			Vec2 normalizedMousePos = EditorGui::mouseToNormalizedViewport();
			OrthoCamera* camera = Application::getEditorCamera();
			Vec2 worldCoords = camera->reverseProject(normalizedMousePos);
			return worldCoords;
		}
	}

	// -------------- Interal Gizmo Functions --------------

	// -------------- Gizmo Functions --------------
	void GizmoState::render()
	{
		GlobalContext* g = GizmoManager::gGizmoManager;
		const OrthoCamera* camera = Application::getEditorCamera();

		// If it's in free move mode, render guidelines
		if (moveMode != FollowMouseMoveMode::None)
		{
			const Vec2& cameraProjectionSize = camera->projectionSize * camera->zoom;
			Vec3 leftGuideline = this->positionMoveStart - Vec3{ cameraProjectionSize.x, 0.0f, 0.0f };
			Vec3 rightGuideline = this->positionMoveStart + Vec3{ cameraProjectionSize.x, 0.0f, 0.0f };
			Vec3 bottomGuideline = this->positionMoveStart - Vec3{ 0.0f, cameraProjectionSize.y, 0.0f };
			Vec3 topGuideline = this->positionMoveStart + Vec3{ 0.0f, cameraProjectionSize.y, 0.0f };

			float hzGuidelineWidth = cameraProjectionSize.x / 1'000.0f;
			float vtGuidelineWidth = cameraProjectionSize.y / 400.0f;
			Vec2 hzGuidelineSize = Vec2{ rightGuideline.x - leftGuideline.x, hzGuidelineWidth };
			Vec2 vtGuidelineSize = Vec2{ vtGuidelineWidth, topGuideline.y - bottomGuideline.y };

			switch (moveMode)
			{
			case FollowMouseMoveMode::HzOnly:
				Renderer::pushColor(Colors::AccentRed[4]);
				Renderer::drawFilledQuad(CMath::vector2From3(this->positionMoveStart), hzGuidelineSize);
				Renderer::popColor();
				break;
			case FollowMouseMoveMode::VtOnly:
				Renderer::pushColor(Colors::AccentGreen[4]);
				Renderer::drawFilledQuad(CMath::vector2From3(this->positionMoveStart), vtGuidelineSize);
				Renderer::popColor();
				break;
			case FollowMouseMoveMode::None:
			case FollowMouseMoveMode::FreeMove:
				break;
			}

			return;
		}

		// Otherwise render the gizmo shapes
		if ((uint8)variant & (uint8)GizmoVariant::Free)
		{
			if ((idHash != g->hoveredGizmo && idHash != g->activeGizmo) || g->hotGizmoVariant != GizmoVariant::Free)
			{
				Renderer::pushColor(Colors::Primary[4]);
				Renderer::drawFilledQuad(CMath::vector2From3(this->position), GizmoManager::defaultFreeMoveSize * camera->zoom);
				Renderer::popColor();
			}
			else if (idHash == g->hoveredGizmo)
			{
				Renderer::pushColor(Colors::Primary[5]);
				Renderer::drawFilledQuad(CMath::vector2From3(this->position), GizmoManager::defaultFreeMoveSize * camera->zoom);
				Renderer::popColor();
			}
			else if (idHash == g->activeGizmo)
			{
				Renderer::pushColor(Colors::Primary[6]);
				Renderer::drawFilledQuad(CMath::vector2From3(this->position), GizmoManager::defaultFreeMoveSize * camera->zoom);
				Renderer::popColor();
			}
		}

		if ((uint8)variant & (uint8)GizmoVariant::Horizontal)
		{
			Vec2 pos = getGizmoPos(this->position, GizmoManager::defaultHorizontalMoveOffset, camera->zoom);
			const Vec4* color = nullptr;
			if ((idHash != g->hoveredGizmo && idHash != g->activeGizmo) || g->hotGizmoVariant != GizmoVariant::Horizontal)
			{
				color = &Colors::AccentRed[4];
			}
			else if (idHash == g->hoveredGizmo)
			{
				color = &Colors::AccentRed[5];
			}
			else if (idHash == g->activeGizmo)
			{
				color = &Colors::AccentRed[6];
			}

			if (color != nullptr)
			{
				Renderer::pushColor(*color);
				Renderer::drawFilledQuad(pos, GizmoManager::defaultHorizontalMoveSize * camera->zoom);
				float stemHalfSize = GizmoManager::defaultHorizontalMoveSize.x / 2.0f;
				Vec2 triP0 = pos + Vec2{ stemHalfSize, GizmoManager::defaultArrowTipHalfWidth } *camera->zoom;
				Vec2 triP1 = pos + Vec2{ stemHalfSize + GizmoManager::defaultArrowTipHeight, 0.0f } *camera->zoom;
				Vec2 triP2 = pos + Vec2{ stemHalfSize, -GizmoManager::defaultArrowTipHalfWidth } *camera->zoom;
				Renderer::drawFilledTri(triP0, triP1, triP2);
				Renderer::popColor();
			}
		}

		if ((uint8)variant & (uint8)GizmoVariant::Vertical)
		{
			Vec2 pos = getGizmoPos(this->position, GizmoManager::defaultVerticalMoveOffset, camera->zoom);
			const Vec4* color = nullptr;
			if ((idHash != g->hoveredGizmo && idHash != g->activeGizmo) || g->hotGizmoVariant != GizmoVariant::Vertical)
			{
				color = &Colors::AccentGreen[4];
			}
			else if (idHash == g->hoveredGizmo)
			{
				color = &Colors::AccentGreen[5];
			}
			else if (idHash == g->activeGizmo)
			{
				color = &Colors::AccentGreen[6];
			}

			if (color != nullptr)
			{
				Renderer::pushColor(*color);
				Renderer::drawFilledQuad(pos, GizmoManager::defaultVerticalMoveSize * camera->zoom);
				float stemHalfSize = GizmoManager::defaultVerticalMoveSize.y / 2.0f;
				Vec2 triP0 = pos + Vec2{ -GizmoManager::defaultArrowTipHalfWidth, stemHalfSize } *camera->zoom;
				Vec2 triP1 = pos + Vec2{ 0.0f, stemHalfSize + GizmoManager::defaultArrowTipHeight } *camera->zoom;
				Vec2 triP2 = pos + Vec2{ GizmoManager::defaultArrowTipHalfWidth, stemHalfSize } *camera->zoom;
				Renderer::drawFilledTri(triP0, triP1, triP2);
				Renderer::popColor();
			}
		}
	}

	// -------------- Interal Gizmo Functions --------------
}