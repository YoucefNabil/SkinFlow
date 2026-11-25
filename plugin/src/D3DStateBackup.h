#pragma once

namespace skinflow
{
	// The output-merger, rasterizer, GS and PS state an extra pass changes, saved on construction
	// and put back by Restore so the game's next draw sees its own state.
	class D3DStateBackup
	{
	public:
		explicit D3DStateBackup(ID3D11DeviceContext* a_context);
		~D3DStateBackup();
		void Restore(ID3D11DeviceContext* a_context);

		D3DStateBackup(const D3DStateBackup&) = delete;
		D3DStateBackup& operator=(const D3DStateBackup&) = delete;

	private:
		std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs{};
		ID3D11DepthStencilView* dsv = nullptr;
		std::array<ID3D11UnorderedAccessView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> uavs{};
		UINT numViewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
		ID3D11RasterizerState* raster = nullptr;
		ID3D11BlendState* blend = nullptr;
		float blendFactor[4]{};
		UINT sampleMask = 0;
		ID3D11DepthStencilState* depth = nullptr;
		UINT stencilRef = 0;
		ID3D11GeometryShader* gs = nullptr;
		ID3D11PixelShader* ps = nullptr;
	};

	// The compute-stage bindings a compute step changes (shader, b0, t0-t1, u0).
	class ComputeStateBackup
	{
	public:
		explicit ComputeStateBackup(ID3D11DeviceContext* a_context);
		~ComputeStateBackup();
		void Restore(ID3D11DeviceContext* a_context);

		ComputeStateBackup(const ComputeStateBackup&) = delete;
		ComputeStateBackup& operator=(const ComputeStateBackup&) = delete;

	private:
		ID3D11ComputeShader* shader = nullptr;
		ID3D11Buffer* constants = nullptr;
		std::array<ID3D11ShaderResourceView*, 2> srvs{};
		ID3D11UnorderedAccessView* uav = nullptr;
	};
}
