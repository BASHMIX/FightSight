#include "App/SpoutReceiverDX.h"

#include "SpoutDX.h"

namespace fightsight {

SpoutReceiverDX::SpoutReceiverDX()
    : m_spout(new spoutDX())
{}

SpoutReceiverDX::~SpoutReceiverDX() {
    Shutdown();
    delete m_spout;
    m_spout = nullptr;
}

bool SpoutReceiverDX::Init(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context || !m_spout) return false;
    m_device  = device;
    m_context = context;
    return m_spout->OpenDirectX11(device);
}

void SpoutReceiverDX::Shutdown() {
    if (m_spout) {
        m_spout->ReleaseReceiver();
        m_spout->CloseDirectX11();
    }
    m_device  = nullptr;
    m_context = nullptr;
}

void SpoutReceiverDX::SetReceiverName(const std::string& name) {
    if (m_spout) m_spout->SetReceiverName(name.c_str());
}

bool SpoutReceiverDX::ReceiveFrame() {
    if (!m_spout || !m_device) return false;
    return m_spout->ReceiveTexture();
}

bool SpoutReceiverDX::IsConnected() const {
    return m_spout && m_spout->IsConnected();
}

unsigned int SpoutReceiverDX::GetWidth() const {
    return m_spout ? m_spout->GetSenderWidth() : 0u;
}

unsigned int SpoutReceiverDX::GetHeight() const {
    return m_spout ? m_spout->GetSenderHeight() : 0u;
}

std::string SpoutReceiverDX::GetSenderName() const {
    if (!m_spout) return {};
    const char* n = m_spout->GetSenderName();
    return n ? std::string(n) : std::string();
}

ID3D11Texture2D* SpoutReceiverDX::GetSenderTexture() const {
    return m_spout ? m_spout->GetSenderTexture() : nullptr;
}

DXGI_FORMAT SpoutReceiverDX::GetSenderFormat() const {
    return m_spout ? m_spout->GetSenderFormat() : DXGI_FORMAT_UNKNOWN;
}

} // namespace fightsight
