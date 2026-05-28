#pragma once

#include <d3d11.h>
#include <dxgiformat.h>
#include <string>

class spoutDX;

namespace fightsight {

// Thin RAII wrapper around Spout2's spoutDX class.
// Receives a shared GPU texture from a Spout sender into our DX11 device.
// The texture is owned by the spoutDX instance; we just hand out a pointer
// for downstream consumers (FrameProcessor) to read from.
class SpoutReceiverDX {
public:
    SpoutReceiverDX();
    ~SpoutReceiverDX();

    SpoutReceiverDX(const SpoutReceiverDX&) = delete;
    SpoutReceiverDX& operator=(const SpoutReceiverDX&) = delete;

    bool Init(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();

    // Empty name = bind to the first available sender.
    void SetReceiverName(const std::string& name);

    // Pull one frame. Call once per render tick. Returns true if a new frame
    // arrived (false if no sender / no new frame this tick).
    bool ReceiveFrame();

    bool IsConnected() const;
    unsigned int GetWidth() const;
    unsigned int GetHeight() const;
    std::string GetSenderName() const;

    // The currently received sender texture. Owned by spoutDX - do NOT release.
    // Valid only while IsConnected() and after at least one ReceiveFrame().
    // Pointer can be invalidated when the sender size/format changes (the
    // following ReceiveFrame() call will return a fresh one).
    ID3D11Texture2D* GetSenderTexture() const;
    DXGI_FORMAT      GetSenderFormat() const;

private:
    ID3D11Device*        m_device  = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    spoutDX*             m_spout   = nullptr;
};

} // namespace fightsight
