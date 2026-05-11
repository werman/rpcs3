#include "stdafx.h"
#include "VulkanAPI.h"

#ifdef ANDROID

#include <cstdlib>
#include <dlfcn.h>

#define VK_ANDROID_FUNC(func) PFN_##func func = nullptr;
#include "VKAndroidProcTable.h"
#undef VK_ANDROID_FUNC

namespace vk
{
	namespace
	{
		void* s_vulkan_library = nullptr;

		template <typename T>
		void load_global(T& out, const char* name)
		{
			out = reinterpret_cast<T>(vkGetInstanceProcAddr(VK_NULL_HANDLE, name));
		}

		template <typename T>
		void load_instance(VkInstance instance, T& out, const char* name)
		{
			if (auto proc = vkGetInstanceProcAddr(instance, name))
			{
				out = reinterpret_cast<T>(proc);
			}
		}

		template <typename T>
		void load_device(VkDevice device, T& out, const char* name)
		{
			if (auto proc = vkGetDeviceProcAddr(device, name))
			{
				out = reinterpret_cast<T>(proc);
			}
		}
	}

	bool init_android_vulkan_loader(std::string* error)
	{
		const char* library = std::getenv("RPCS3_VULKAN_LIBRARY");
		if (!library || !*library)
		{
			library = "libvulkan.so";
		}

		s_vulkan_library = dlopen(library, RTLD_NOW | RTLD_LOCAL);
		if (!s_vulkan_library)
		{
			if (error)
			{
				*error = fmt::format("Failed to load Vulkan loader '%s': %s", library, dlerror());
			}

			return false;
		}

		vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(s_vulkan_library, "vkGetInstanceProcAddr"));
		if (!vkGetInstanceProcAddr)
		{
			if (error)
			{
				*error = fmt::format("Failed to resolve vkGetInstanceProcAddr from Vulkan loader '%s'", library);
			}

			return false;
		}

		load_global(vkCreateInstance, "vkCreateInstance");
		load_global(vkEnumerateInstanceExtensionProperties, "vkEnumerateInstanceExtensionProperties");
		load_global(vkGetDeviceProcAddr, "vkGetDeviceProcAddr");

		if (!vkCreateInstance || !vkEnumerateInstanceExtensionProperties)
		{
			if (error)
			{
				*error = fmt::format("Vulkan loader '%s' does not expose required global entrypoints", library);
			}

			return false;
		}

		return true;
	}

	void load_android_vulkan_instance_procs(VkInstance instance)
	{
#define VK_ANDROID_FUNC(func) load_instance(instance, func, #func);
#include "VKAndroidProcTable.h"
#undef VK_ANDROID_FUNC
	}

	void load_android_vulkan_device_procs(VkDevice device)
	{
#define VK_ANDROID_FUNC(func) load_device(device, func, #func);
#include "VKAndroidProcTable.h"
#undef VK_ANDROID_FUNC
	}
}

#endif
