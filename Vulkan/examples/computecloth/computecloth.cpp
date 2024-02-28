/*
* Vulkan Example - Compute shader cloth simulation
*
* A compute shader updates a shader storage buffer that contains particles held together by springs and also does basic
* collision detection against a sphere. This storage buffer is then used as the vertex input for the graphics part of the sample
*
* Copyright (C) 2016-2023 by Sascha Willems - www.saschawillems.de
*
* This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
*/

#include "vulkanexamplebase.h"
#include "VulkanglTFModel.h"


class VulkanExample : public VulkanExampleBase
{
public:
	uint32_t readSet{ 0 };
	uint32_t indexCount{ 0 };
	bool simulateWind{ false };
	// This will be set to true, if the device has a dedicated queue from a compute only queue family
	// With such a queue graphics and compute workloads can run in parallel, but this also requires additional barriers (often called "async compute")
	// These barriers will release and acquire the resources used in graphics and compute between the different queue families
	bool dedicatedComputeQueue{ false };

	vks::Texture2D textureCloth;
	vkglTF::Model modelSphere;

	// The cloth is made from a grid of particles
	struct Particle {
		glm::vec4 pos;
		glm::vec4 vel;
		glm::vec4 uv;
		glm::vec4 normal;
	};

	// Cloth definition parameters
	struct Cloth {
		glm::uvec2 gridsize{ 60, 60 };
		glm::vec2 size{ 5.0f, 5.0f };
	} cloth;

	// We put the resource "types" into structs to make this sample easier to understand

	// We use two buffers for our cloth simulation: One with the input cloth data and one for outputting updated values
	// The compute pipeline will update the output buffer, and the graphics pipeline will it as a vertex buffer
	struct StorageBuffers {
		vks::Buffer input;
		vks::Buffer output;
	} storageBuffers;

	// Resources for the graphics part of the example
	struct Graphics {
		VkDescriptorSetLayout descriptorSetLayout{ VK_NULL_HANDLE };
		VkDescriptorSet descriptorSet{ VK_NULL_HANDLE };
		VkPipelineLayout pipelineLayout{ VK_NULL_HANDLE };
		struct Pipelines {
			VkPipeline cloth{ VK_NULL_HANDLE };
			VkPipeline sphere{ VK_NULL_HANDLE };
		} pipelines;
		// The vertices will be stored in the shader storage buffers, so we only need an index buffer in this structure
		vks::Buffer indices;
		struct UniformData {
			glm::mat4 projection;
			glm::mat4 view;
			glm::vec4 lightPos{ -2.0f, 4.0f, -2.0f, 1.0f };
		} uniformData;
		vks::Buffer uniformBuffer;
	} graphics;

	// Resources for the compute part of the example
	struct Compute {
		struct Semaphores {
			VkSemaphore ready{ VK_NULL_HANDLE };
			VkSemaphore complete{ VK_NULL_HANDLE };
		} semaphores;
		VkQueue queue{ VK_NULL_HANDLE };
		VkCommandPool commandPool{ VK_NULL_HANDLE };
		std::array<VkCommandBuffer, 2> commandBuffers{};
		VkDescriptorSetLayout descriptorSetLayout{ VK_NULL_HANDLE };
		std::array<VkDescriptorSet, 2> descriptorSets{ VK_NULL_HANDLE };
		VkPipelineLayout pipelineLayout{ VK_NULL_HANDLE };
		VkPipeline pipeline{ VK_NULL_HANDLE };
		struct UniformData {
			float deltaT{ 0.0f };
			// These arguments define the spring setup for the cloth piece
			// Changing these changes how the cloth reacts
			float particleMass{ 0.1f };
			float springStiffness{ 2000.0f };
			float damping{ 0.25f };
			float restDistH{ 0 };
			float restDistV{ 0 };
			float restDistD{ 0 };
			float sphereRadius{ 1.0f };
			glm::vec4 spherePos{ 0.0f, 0.0f, 0.0f, 0.0f };
			glm::vec4 gravity{ 0.0f, 9.8f, 0.0f, 0.0f };
			glm::ivec2 particleCount{ 0 };
		} uniformData;
		vks::Buffer uniformBuffer;
	} compute;

	VulkanExample() : VulkanExampleBase()
	{
		title = "Compute shader cloth simulation";
		camera.type = Camera::CameraType::lookat;
		camera.setPerspective(60.0f, (float)width / (float)height, 0.1f, 512.0f);
		camera.setRotation(glm::vec3(-30.0f, -45.0f, 0.0f));
		camera.setTranslation(glm::vec3(0.0f, 0.0f, -5.0f));
	}

	~VulkanExample()
	{
		if (device) {
			// Graphics
			graphics.indices.destroy();
			graphics.uniformBuffer.destroy();
			vkDestroyPipeline(device, graphics.pipelines.cloth, nullptr);
			vkDestroyPipeline(device, graphics.pipelines.sphere, nullptr);
			vkDestroyPipelineLayout(device, graphics.pipelineLayout, nullptr);
			vkDestroyDescriptorSetLayout(device, graphics.descriptorSetLayout, nullptr);
			textureCloth.destroy();

			// Compute
			compute.uniformBuffer.destroy();
			vkDestroyPipelineLayout(device, compute.pipelineLayout, nullptr);
			vkDestroyDescriptorSetLayout(device, compute.descriptorSetLayout, nullptr);
			vkDestroyPipeline(device, compute.pipeline, nullptr);
			vkDestroySemaphore(device, compute.semaphores.ready, nullptr);
			vkDestroySemaphore(device, compute.semaphores.complete, nullptr);
			vkDestroyCommandPool(device, compute.commandPool, nullptr);

			// SSBOs
			storageBuffers.input.destroy();
			storageBuffers.output.destroy();
		}
	}

	// Enable physical device features required for this example
	virtual void getEnabledFeatures()
	{
		if (deviceFeatures.samplerAnisotropy) {
			enabledFeatures.samplerAnisotropy = VK_TRUE;
		}
	};

	void loadAssets()
	{
		const uint32_t glTFLoadingFlags = vkglTF::FileLoadingFlags::PreTransformVertices | vkglTF::FileLoadingFlags::PreMultiplyVertexColors | vkglTF::FileLoadingFlags::FlipY;
		modelSphere.loadFromFile(getAssetPath() + "models/sphere.gltf", vulkanDevice, queue, glTFLoadingFlags);
		textureCloth.loadFromFile(getAssetPath() + "textures/vulkan_cloth_rgba.ktx", VK_FORMAT_R8G8B8A8_UNORM, vulkanDevice, queue);
	}

	void addGraphicsToComputeBarriers(VkCommandBuffer commandBuffer, VkAccessFlags srcAccessMask, VkAccessFlags dstAccessMask, VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask)
	{
		if (dedicatedComputeQueue) {
			VkBufferMemoryBarrier bufferBarrier = vks::initializers::bufferMemoryBarrier();
			bufferBarrier.srcAccessMask = srcAccessMask;
			bufferBarrier.dstAccessMask = dstAccessMask;
			bufferBarrier.srcQueueFamilyIndex = vulkanDevice->queueFamilyIndices.graphics;
			bufferBarrier.dstQueueFamilyIndex = vulkanDevice->queueFamilyIndices.compute;
			bufferBarrier.size = VK_WHOLE_SIZE;

			std::vector<VkBufferMemoryBarrier> bufferBarriers;
			bufferBarrier.buffer = storageBuffers.input.buffer;
			bufferBarriers.push_back(bufferBarrier);
			bufferBarrier.buffer = storageBuffers.output.buffer;
			bufferBarriers.push_back(bufferBarrier);
			vkCmdPipelineBarrier(commandBuffer,
				srcStageMask,
				dstStageMask,
				VK_FLAGS_NONE,
				0, nullptr,
				static_cast<uint32_t>(bufferBarriers.size()), bufferBarriers.data(),
				0, nullptr);
		}
	}

	void addComputeToComputeBarriers(VkCommandBuffer commandBuffer)
	{
		VkBufferMemoryBarrier bufferBarrier = vks::initializers::bufferMemoryBarrier();
		bufferBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		bufferBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bufferBarrier.size = VK_WHOLE_SIZE;
		std::vector<VkBufferMemoryBarrier> bufferBarriers;
		bufferBarrier.buffer = storageBuffers.input.buffer;
		bufferBarriers.push_back(bufferBarrier);
		bufferBarrier.buffer = storageBuffers.output.buffer;
		bufferBarriers.push_back(bufferBarrier);
		vkCmdPipelineBarrier(
			commandBuffer,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_FLAGS_NONE,
			0, nullptr,
			static_cast<uint32_t>(bufferBarriers.size()), bufferBarriers.data(),
			0, nullptr);
	}

	void addComputeToGraphicsBarriers(VkCommandBuffer commandBuffer, VkAccessFlags srcAccessMask, VkAccessFlags dstAccessMask, VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask)
	{
		if (dedicatedComputeQueue) {
			VkBufferMemoryBarrier bufferBarrier = vks::initializers::bufferMemoryBarrier();
			bufferBarrier.srcAccessMask = srcAccessMask;
			bufferBarrier.dstAccessMask = dstAccessMask;
			bufferBarrier.srcQueueFamilyIndex = vulkanDevice->queueFamilyIndices.compute;
			bufferBarrier.dstQueueFamilyIndex = vulkanDevice->queueFamilyIndices.graphics;
			bufferBarrier.size = VK_WHOLE_SIZE;
			std::vector<VkBufferMemoryBarrier> bufferBarriers;
			bufferBarrier.buffer = storageBuffers.input.buffer;
			bufferBarriers.push_back(bufferBarrier);
			bufferBarrier.buffer = storageBuffers.output.buffer;
			bufferBarriers.push_back(bufferBarrier);
			vkCmdPipelineBarrier(
				commandBuffer,
				srcStageMask,
				dstStageMask,
				VK_FLAGS_NONE,
				0, nullptr,
				static_cast<uint32_t>(bufferBarriers.size()), bufferBarriers.data(),
				0, nullptr);
		}
	}

	void buildCommandBuffers()
	{
		VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();

		VkClearValue clearValues[2];
		clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
		clearValues[1].depthStencil = { 1.0f, 0 };

		VkRenderPassBeginInfo renderPassBeginInfo = vks::initializers::renderPassBeginInfo();
		renderPassBeginInfo.renderPass = renderPass;
		renderPassBeginInfo.renderArea.offset.x = 0;
		renderPassBeginInfo.renderArea.offset.y = 0;
		renderPassBeginInfo.renderArea.extent.width = width;
		renderPassBeginInfo.renderArea.extent.height = height;
		renderPassBeginInfo.clearValueCount = 2;
		renderPassBeginInfo.pClearValues = clearValues;

		for (int32_t i = 0; i < drawCmdBuffers.size(); ++i)
		{
			// Set target frame buffer
			renderPassBeginInfo.framebuffer = frameBuffers[i];

			VK_CHECK_RESULT(vkBeginCommandBuffer(drawCmdBuffers[i], &cmdBufInfo));

			// Acquire storage buffers from compute queue
			addComputeToGraphicsBarriers(drawCmdBuffers[i], 0, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);

			// Draw the particle system using the update vertex buffer

			vkCmdBeginRenderPass(drawCmdBuffers[i], &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

			VkViewport viewport = vks::initializers::viewport((float)width, (float)height, 0.0f, 1.0f);
			vkCmdSetViewport(drawCmdBuffers[i], 0, 1, &viewport);

			VkRect2D scissor = vks::initializers::rect2D(width, height, 0, 0);
			vkCmdSetScissor(drawCmdBuffers[i], 0, 1, &scissor);

			VkDeviceSize offsets[1] = { 0 };

			// Render sphere
			vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, graphics.pipelines.sphere);
			vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, graphics.pipelineLayout, 0, 1, &graphics.descriptorSet, 0, NULL);
			modelSphere.draw(drawCmdBuffers[i]);

			// Render cloth
			vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, graphics.pipelines.cloth);
			vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, graphics.pipelineLayout, 0, 1, &graphics.descriptorSet, 0, NULL);
			vkCmdBindIndexBuffer(drawCmdBuffers[i], graphics.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
			vkCmdBindVertexBuffers(drawCmdBuffers[i], 0, 1, &storageBuffers.output.buffer, offsets);
			vkCmdDrawIndexed(drawCmdBuffers[i], indexCount, 1, 0, 0, 0);

			drawUI(drawCmdBuffers[i]);

			vkCmdEndRenderPass(drawCmdBuffers[i]);

			// release the storage buffers to the compute queue
			addGraphicsToComputeBarriers(drawCmdBuffers[i], VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, 0, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

			VK_CHECK_RESULT(vkEndCommandBuffer(drawCmdBuffers[i]));
		}

	}

	void buildComputeCommandBuffer()
	{
		VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();
		cmdBufInfo.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;

		for (uint32_t i = 0; i < 2; i++) {

			VK_CHECK_RESULT(vkBeginCommandBuffer(compute.commandBuffers[i], &cmdBufInfo));

			// Acquire the storage buffers from the graphics queue
			addGraphicsToComputeBarriers(compute.commandBuffers[i], 0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

			vkCmdBindPipeline(compute.commandBuffers[i], VK_PIPELINE_BIND_POINT_COMPUTE, compute.pipeline);

			uint32_t calculateNormals = 0;
			vkCmdPushConstants(compute.commandBuffers[i], compute.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t), &calculateNormals);

			// Dispatch the compute job
			const uint32_t iterations = 64;
			for (uint32_t j = 0; j < iterations; j++) {
				readSet = 1 - readSet;
				vkCmdBindDescriptorSets(compute.commandBuffers[i], VK_PIPELINE_BIND_POINT_COMPUTE, compute.pipelineLayout, 0, 1, &compute.descriptorSets[readSet], 0, 0);

				if (j == iterations - 1) {
					calculateNormals = 1;
					vkCmdPushConstants(compute.commandBuffers[i], compute.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t), &calculateNormals);
				}

				vkCmdDispatch(compute.commandBuffers[i], cloth.gridsize.x / 10, cloth.gridsize.y / 10, 1);

				// Don't add a barrier on the last iteration of the loop, since we'll have an explicit release to the graphics queue
				if (j != iterations - 1) {
					addComputeToComputeBarriers(compute.commandBuffers[i]);
				}

			}

			// release the storage buffers back to the graphics queue
			addComputeToGraphicsBarriers(compute.commandBuffers[i], VK_ACCESS_SHADER_WRITE_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
			vkEndCommandBuffer(compute.commandBuffers[i]);
		}
	}

	// Setup and fill the shader storage buffers containing the particles
	// These buffers are used as shader storage buffers in the compute shader (to update them) and as vertex input in the vertex shader (to display them)
	void prepareStorageBuffers()
	{
		std::vector<Particle> particleBuffer(cloth.gridsize.x * cloth.gridsize.y);

		float dx = cloth.size.x / (cloth.gridsize.x - 1);
		float dy = cloth.size.y / (cloth.gridsize.y - 1);
		float du = 1.0f / (cloth.gridsize.x - 1);
		float dv = 1.0f / (cloth.gridsize.y - 1);

		// Set up a flat cloth that falls onto sphere
		glm::mat4 transM = glm::translate(glm::mat4(1.0f), glm::vec3(-cloth.size.x / 2.0f, -2.0f, -cloth.size.y / 2.0f));
		for (uint32_t i = 0; i < cloth.gridsize.y; i++) {
			for (uint32_t j = 0; j < cloth.gridsize.x; j++) {
				particleBuffer[i + j * cloth.gridsize.y].pos = transM * glm::vec4(dx * j, 0.0f, dy * i, 1.0f);
				particleBuffer[i + j * cloth.gridsize.y].vel = glm::vec4(0.0f);
				particleBuffer[i + j * cloth.gridsize.y].uv = glm::vec4(1.0f - du * i, dv * j, 0.0f, 0.0f);
			}
		}

		VkDeviceSize storageBufferSize = particleBuffer.size() * sizeof(Particle);

		// Staging
		// SSBO won't be changed on the host after upload so copy to device local memory

		vks::Buffer stagingBuffer;

		vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&stagingBuffer,
			storageBufferSize,
			particleBuffer.data());

		// SSBOs will be used both as storage buffers (compute) and vertex buffers (graphics)
		vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			&storageBuffers.input,
			storageBufferSize);

		vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			&storageBuffers.output,
			storageBufferSize);

		// Copy from staging buffer
		VkCommandBuffer copyCmd = vulkanDevice->createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);
		VkBufferCopy copyRegion = {};
		copyRegion.size = storageBufferSize;
		vkCmdCopyBuffer(copyCmd, stagingBuffer.buffer, storageBuffers.output.buffer, 1, &copyRegion);
		// Add an initial release barrier to the graphics queue,
		// so that when the compute command buffer executes for the first time
		// it doesn't complain about a lack of a corresponding "release" to its "acquire"
		addGraphicsToComputeBarriers(copyCmd, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, 0, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
		vulkanDevice->flushCommandBuffer(copyCmd, queue, true);

		stagingBuffer.destroy();

		// Indices
		std::vector<uint32_t> indices;
		for (uint32_t y = 0; y < cloth.gridsize.y - 1; y++) {
			for (uint32_t x = 0; x < cloth.gridsize.x; x++) {
				indices.push_back((y + 1) * cloth.gridsize.x + x);
				indices.push_back((y)*cloth.gridsize.x + x);
			}
			// Primitive restart (signaled by special value 0xFFFFFFFF)
			indices.push_back(0xFFFFFFFF);
		}
		uint32_t indexBufferSize = static_cast<uint32_t>(indices.size()) * sizeof(uint32_t);
		indexCount = static_cast<uint32_t>(indices.size());

		vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&stagingBuffer,
			indexBufferSize,
			indices.data());

		vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			&graphics.indices,
			indexBufferSize);

		// Copy from staging buffer
		copyCmd = vulkanDevice->createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);
		copyRegion = {};
		copyRegion.size = indexBufferSize;
		vkCmdCopyBuffer(copyCmd, stagingBuffer.buffer, graphics.indices.buffer, 1, &copyRegion);
		vulkanDevice->flushCommandBuffer(copyCmd, queue, true);

		stagingBuffer.destroy();
	}

	// Prepare the resources used for the graphics part of the sample
	void prepareGraphics()
	{
		// Uniform buffer for passing data to the vertex shader
		vulkanDevice->createBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &graphics.uniformBuffer, sizeof(Graphics::UniformData));
		VK_CHECK_RESULT(graphics.uniformBuffer.map());

		// Descriptor pool
		std::vector<VkDescriptorPoolSize> poolSizes = {
			vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3),
			vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4),
			vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2)
		};
		VkDescriptorPoolCreateInfo descriptorPoolInfo = vks::initializers::descriptorPoolCreateInfo(poolSizes, 3);
		VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr, &descriptorPool));

		// Descriptor layout
		std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings = {
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT, 0),
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 1)
		};
		VkDescriptorSetLayoutCreateInfo descriptorLayout = vks::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings);
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorLayout, nullptr, &graphics.descriptorSetLayout));

		// Decscriptor set
		VkDescriptorSetAllocateInfo allocInfo = vks::initializers::descriptorSetAllocateInfo(descriptorPool, &graphics.descriptorSetLayout, 1);
		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &graphics.descriptorSet));
		std::vector<VkWriteDescriptorSet> writeDescriptorSets = {
			vks::initializers::writeDescriptorSet(graphics.descriptorSet, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, &graphics.uniformBuffer.descriptor),
			vks::initializers::writeDescriptorSet(graphics.descriptorSet, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, &textureCloth.descriptor)
		};
		vkUpdateDescriptorSets(device, static_cast<uint32_t>(writeDescriptorSets.size()), writeDescriptorSets.data(), 0, nullptr);

		// Layout
		VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = vks::initializers::pipelineLayoutCreateInfo(&graphics.descriptorSetLayout, 1);
		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &graphics.pipelineLayout));

		// Pipeline
		VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = vks::initializers::pipelineInputAssemblyStateCreateInfo(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 0, VK_TRUE);
		VkPipelineRasterizationStateCreateInfo rasterizationState = vks::initializers::pipelineRasterizationStateCreateInfo(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0);
		VkPipelineColorBlendAttachmentState blendAttachmentState = vks::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE);
		VkPipelineColorBlendStateCreateInfo colorBlendState = vks::initializers::pipelineColorBlendStateCreateInfo(1, &blendAttachmentState);
		VkPipelineDepthStencilStateCreateInfo depthStencilState = vks::initializers::pipelineDepthStencilStateCreateInfo(VK_TRUE, VK_TRUE, VK_COMPARE_OP_LESS_OR_EQUAL);
		VkPipelineViewportStateCreateInfo viewportState = vks::initializers::pipelineViewportStateCreateInfo(1, 1, 0);
		VkPipelineMultisampleStateCreateInfo multisampleState = vks::initializers::pipelineMultisampleStateCreateInfo(VK_SAMPLE_COUNT_1_BIT, 0);
		std::vector<VkDynamicState> dynamicStateEnables = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamicState = vks::initializers::pipelineDynamicStateCreateInfo(dynamicStateEnables);

		// Rendering pipeline
		std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages;

		shaderStages[0] = loadShader(getShadersPath() + "computecloth/cloth.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		shaderStages[1] = loadShader(getShadersPath() + "computecloth/cloth.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);

		VkGraphicsPipelineCreateInfo pipelineCreateInfo = vks::initializers::pipelineCreateInfo(graphics.pipelineLayout, renderPass);

		// Vertex Input
		std::vector<VkVertexInputBindingDescription> inputBindings = {
			vks::initializers::vertexInputBindingDescription(0, sizeof(Particle), VK_VERTEX_INPUT_RATE_VERTEX)
		};
		// Attribute descriptions based on the particles of the cloth
		std::vector<VkVertexInputAttributeDescription> inputAttributes = {
			vks::initializers::vertexInputAttributeDescription(0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Particle, pos)),
			vks::initializers::vertexInputAttributeDescription(0, 1, VK_FORMAT_R32G32_SFLOAT, offsetof(Particle, uv)),
			vks::initializers::vertexInputAttributeDescription(0, 2, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Particle, normal))
		};

		// Assign to vertex buffer
		VkPipelineVertexInputStateCreateInfo inputState = vks::initializers::pipelineVertexInputStateCreateInfo();
		inputState.vertexBindingDescriptionCount = static_cast<uint32_t>(inputBindings.size());
		inputState.pVertexBindingDescriptions = inputBindings.data();
		inputState.vertexAttributeDescriptionCount = static_cast<uint32_t>(inputAttributes.size());
		inputState.pVertexAttributeDescriptions = inputAttributes.data();

		pipelineCreateInfo.pVertexInputState = &inputState;
		pipelineCreateInfo.pInputAssemblyState = &inputAssemblyState;
		pipelineCreateInfo.pRasterizationState = &rasterizationState;
		pipelineCreateInfo.pColorBlendState = &colorBlendState;
		pipelineCreateInfo.pMultisampleState = &multisampleState;
		pipelineCreateInfo.pViewportState = &viewportState;
		pipelineCreateInfo.pDepthStencilState = &depthStencilState;
		pipelineCreateInfo.pDynamicState = &dynamicState;
		pipelineCreateInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
		pipelineCreateInfo.pStages = shaderStages.data();
		pipelineCreateInfo.renderPass = renderPass;
		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCreateInfo, nullptr, &graphics.pipelines.cloth));

		// Sphere rendering pipeline
		pipelineCreateInfo.pVertexInputState = vkglTF::Vertex::getPipelineVertexInputState({ vkglTF::VertexComponent::Position, vkglTF::VertexComponent::UV, vkglTF::VertexComponent::Normal });
		inputState.vertexAttributeDescriptionCount = static_cast<uint32_t>(inputAttributes.size());
		inputAssemblyState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		inputAssemblyState.primitiveRestartEnable = VK_FALSE;
		rasterizationState.polygonMode = VK_POLYGON_MODE_FILL;
		shaderStages[0] = loadShader(getShadersPath() + "computecloth/sphere.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		shaderStages[1] = loadShader(getShadersPath() + "computecloth/sphere.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCreateInfo, nullptr, &graphics.pipelines.sphere));

		buildCommandBuffers();
	}

	// Prepare the resources used for the compute part of the sample
	void prepareCompute()
	{
		// Create a compute capable device queue
		vkGetDeviceQueue(device, vulkanDevice->queueFamilyIndices.compute, 0, &compute.queue);

		// Uniform buffer for passing data to the compute shader
		vulkanDevice->createBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &compute.uniformBuffer, sizeof(Compute::UniformData));
		VK_CHECK_RESULT(compute.uniformBuffer.map());

		// Set some initial values
		float dx = cloth.size.x / (cloth.gridsize.x - 1);
		float dy = cloth.size.y / (cloth.gridsize.y - 1);

		compute.uniformData.restDistH = dx;
		compute.uniformData.restDistV = dy;
		compute.uniformData.restDistD = sqrtf(dx * dx + dy * dy);
		compute.uniformData.particleCount = cloth.gridsize;

		// Create compute pipeline
		std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings = {
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT, 0),
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT, 1),
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT, 2),
		};

		VkDescriptorSetLayoutCreateInfo descriptorLayout = vks::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings);
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorLayout, nullptr, &compute.descriptorSetLayout));

		VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = vks::initializers::pipelineLayoutCreateInfo(&compute.descriptorSetLayout, 1);

		// Push constants used to pass some parameters
		VkPushConstantRange pushConstantRange = vks::initializers::pushConstantRange(VK_SHADER_STAGE_COMPUTE_BIT, sizeof(uint32_t), 0);
		pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
		pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantRange;
		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &compute.pipelineLayout));

		VkDescriptorSetAllocateInfo allocInfo = vks::initializers::descriptorSetAllocateInfo(descriptorPool, &compute.descriptorSetLayout, 1);

		// Create two descriptor sets with input and output buffers switched
		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &compute.descriptorSets[0]));
		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &compute.descriptorSets[1]));

		std::vector<VkWriteDescriptorSet> computeWriteDescriptorSets = {
			vks::initializers::writeDescriptorSet(compute.descriptorSets[0], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &storageBuffers.input.descriptor),
			vks::initializers::writeDescriptorSet(compute.descriptorSets[0], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, &storageBuffers.output.descriptor),
			vks::initializers::writeDescriptorSet(compute.descriptorSets[0], VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2, &compute.uniformBuffer.descriptor),

			vks::initializers::writeDescriptorSet(compute.descriptorSets[1], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &storageBuffers.output.descriptor),
			vks::initializers::writeDescriptorSet(compute.descriptorSets[1], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, &storageBuffers.input.descriptor),
			vks::initializers::writeDescriptorSet(compute.descriptorSets[1], VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2, &compute.uniformBuffer.descriptor)
		};

		vkUpdateDescriptorSets(device, static_cast<uint32_t>(computeWriteDescriptorSets.size()), computeWriteDescriptorSets.data(), 0, NULL);

		// Create pipeline
		VkComputePipelineCreateInfo computePipelineCreateInfo = vks::initializers::computePipelineCreateInfo(compute.pipelineLayout, 0);
		computePipelineCreateInfo.stage = loadShader(getShadersPath() + "computecloth/cloth.comp.spv", VK_SHADER_STAGE_COMPUTE_BIT);
		VK_CHECK_RESULT(vkCreateComputePipelines(device, pipelineCache, 1, &computePipelineCreateInfo, nullptr, &compute.pipeline));

		// Separate command pool as queue family for compute may be different than graphics
		VkCommandPoolCreateInfo cmdPoolInfo = {};
		cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		cmdPoolInfo.queueFamilyIndex = vulkanDevice->queueFamilyIndices.compute;
		cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		VK_CHECK_RESULT(vkCreateCommandPool(device, &cmdPoolInfo, nullptr, &compute.commandPool));

		// Create a command buffer for compute operations
		VkCommandBufferAllocateInfo cmdBufAllocateInfo = vks::initializers::commandBufferAllocateInfo(compute.commandPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2);
		VK_CHECK_RESULT(vkAllocateCommandBuffers(device, &cmdBufAllocateInfo, &compute.commandBuffers[0]));

		// Semaphores for graphics / compute synchronization
		VkSemaphoreCreateInfo semaphoreCreateInfo = vks::initializers::semaphoreCreateInfo();
		VK_CHECK_RESULT(vkCreateSemaphore(device, &semaphoreCreateInfo, nullptr, &compute.semaphores.ready));
		VK_CHECK_RESULT(vkCreateSemaphore(device, &semaphoreCreateInfo, nullptr, &compute.semaphores.complete));

		// Build a single command buffer containing the compute dispatch commands
		buildComputeCommandBuffer();
	}

	void updateComputeUBO()
	{
		if (!paused) {
			// SRS - Clamp frameTimer to max 20ms refresh period (e.g. if blocked on resize), otherwise image breakup can occur
			compute.uniformData.deltaT = fmin(frameTimer, 0.02f) * 0.0025f;

			if (simulateWind) {
				std::default_random_engine rndEngine(benchmark.active ? 0 : (unsigned)time(nullptr));
				std::uniform_real_distribution<float> rd(1.0f, 12.0f);
				compute.uniformData.gravity.x = cos(glm::radians(-timer * 360.0f)) * (rd(rndEngine) - rd(rndEngine));
				compute.uniformData.gravity.z = sin(glm::radians(timer * 360.0f)) * (rd(rndEngine) - rd(rndEngine));
			}
			else {
				compute.uniformData.gravity.x = 0.0f;
				compute.uniformData.gravity.z = 0.0f;
			}
		}
		else {
			compute.uniformData.deltaT = 0.0f;
		}
		memcpy(compute.uniformBuffer.mapped, &compute.uniformData, sizeof(Compute::UniformData));
	}

	void updateGraphicsUBO()
	{
		graphics.uniformData.projection = camera.matrices.perspective;
		graphics.uniformData.view = camera.matrices.view;
		memcpy(graphics.uniformBuffer.mapped, &graphics.uniformData, sizeof(Graphics::UniformData));
	}

	void draw()
	{
		// As we use both graphics and compute, frame submission is a bit more involved
		// We'll be using semaphores to synchronize between the compute shader updating the cloth and the graphics pipeline drawing it

		static bool firstDraw = true;
		VkSubmitInfo computeSubmitInfo = vks::initializers::submitInfo();
		VkPipelineStageFlags computeWaitDstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		if (!firstDraw) {
			computeSubmitInfo.waitSemaphoreCount = 1;
			computeSubmitInfo.pWaitSemaphores = &compute.semaphores.ready;
			computeSubmitInfo.pWaitDstStageMask = &computeWaitDstStageMask;
		}
		else {
			firstDraw = false;
		}
		computeSubmitInfo.signalSemaphoreCount = 1;
		computeSubmitInfo.pSignalSemaphores = &compute.semaphores.complete;
		computeSubmitInfo.commandBufferCount = 1;
		computeSubmitInfo.pCommandBuffers = &compute.commandBuffers[readSet];

		VK_CHECK_RESULT(vkQueueSubmit(compute.queue, 1, &computeSubmitInfo, VK_NULL_HANDLE));

		// Submit graphics commands
		VulkanExampleBase::prepareFrame();

		VkPipelineStageFlags waitDstStageMask[2] = {
			submitPipelineStages, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT
		};
		VkSemaphore waitSemaphores[2] = {
			semaphores.presentComplete, compute.semaphores.complete
		};
		VkSemaphore signalSemaphores[2] = {
			semaphores.renderComplete, compute.semaphores.ready
		};

		submitInfo.waitSemaphoreCount = 2;
		submitInfo.pWaitDstStageMask = waitDstStageMask;
		submitInfo.pWaitSemaphores = waitSemaphores;
		submitInfo.signalSemaphoreCount = 2;
		submitInfo.pSignalSemaphores = signalSemaphores;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &drawCmdBuffers[currentBuffer];
		VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));

		VulkanExampleBase::submitFrame();
	}

	void prepare()
	{
		VulkanExampleBase::prepare();
		// Make sure the code works properly both with different queues families for graphics and compute and the same queue family
		// You can use DEBUG_FORCE_SHARED_GRAPHICS_COMPUTE_QUEUE preprocessor define to force graphics and compute from the same queue family 
#ifdef DEBUG_FORCE_SHARED_GRAPHICS_COMPUTE_QUEUE
		vulkanDevice->queueFamilyIndices.compute = vulkanDevice->queueFamilyIndices.graphics;
#endif
		// Check whether the compute queue family is distinct from the graphics queue family
		dedicatedComputeQueue = vulkanDevice->queueFamilyIndices.graphics != vulkanDevice->queueFamilyIndices.compute;
		loadAssets();
		prepareStorageBuffers();
		prepareGraphics();
		prepareCompute();
		prepared = true;
	}

	virtual void render()
	{
		if (!prepared)
			return;
		updateGraphicsUBO();
		updateComputeUBO();
		draw();
	}

	virtual void OnUpdateUIOverlay(vks::UIOverlay* overlay)
	{
		if (overlay->header("Settings")) {
			overlay->checkBox("Simulate wind", &simulateWind);
		}
	}
};

VULKAN_EXAMPLE_MAIN()
