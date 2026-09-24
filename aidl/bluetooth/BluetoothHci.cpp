/*
 * Copyright 2022 The Android Open Source Project
 * Copyright 2024-2025 NXP
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "android.hardware.bluetooth.service.mediatek"

#include "BluetoothHci.h"

#include <cutils/properties.h>

#include "log/log.h"
#include "vendor_interface.h"

using namespace ::android::hardware::bluetooth::hci;
using namespace ::android::hardware::bluetooth::async;
using aidl::android::hardware::bluetooth::Status;

namespace aidl::android::hardware::bluetooth::impl {

void OnDeath(void* cookie);
std::optional<std::string> GetSystemProperty(const std::string& property) {
    std::array<char, PROPERTY_VALUE_MAX> value_array{0};
    auto value_len = property_get(property.c_str(), value_array.data(), nullptr);
    if (value_len <= 0) {
        return std::nullopt;
    }
    return std::string(value_array.data(), value_len);
}
bool starts_with(const std::string& str, const std::string& prefix) {
    return str.compare(0, prefix.length(), prefix) == 0;
}

class BluetoothDeathRecipient {
  public:
    BluetoothDeathRecipient(BluetoothHci* hci) : mHci(hci) {}

    bool LinkToDeath(const std::shared_ptr<IBluetoothHciCallbacks>& cb) {
        AIBinder_DeathRecipient* oldDeathRecipient = nullptr;
        {
            std::lock_guard<std::mutex> guard(mMutex);
            oldDeathRecipient = clientDeathRecipient_;
            clientDeathRecipient_ = nullptr;
            mCb.reset();
        }
        if (oldDeathRecipient != nullptr) {
            AIBinder_DeathRecipient_delete(oldDeathRecipient);
        }

        auto* deathRecipient = AIBinder_DeathRecipient_new(OnDeath);
        AIBinder_DeathRecipient_setOnUnlinked(deathRecipient, [](void*) {});
        {
            std::lock_guard<std::mutex> guard(mMutex);
            clientDeathRecipient_ = deathRecipient;
            mCb = cb;
            has_died_ = false;
        }

        auto linkToDeathReturnStatus =
                AIBinder_linkToDeath(cb->asBinder().get(), deathRecipient, this /* cookie */);
        if (linkToDeathReturnStatus != STATUS_OK) {
            Reset();
            ALOGE("Unable to link to death recipient: %d", linkToDeathReturnStatus);
            return false;
        }
        return true;
    }

    void UnlinkToDeath(const std::shared_ptr<IBluetoothHciCallbacks>& cb) {
        AIBinder_DeathRecipient* deathRecipient = nullptr;
        {
            std::lock_guard<std::mutex> guard(mMutex);
            if (cb != mCb) {
                ALOGW("Unable to unlink mismatched pointers");
                return;
            }
            deathRecipient = clientDeathRecipient_;
            clientDeathRecipient_ = nullptr;
            mCb.reset();
        }

        if (deathRecipient == nullptr || cb == nullptr) {
            return;
        }

        auto unlinkToDeathReturnStatus =
                AIBinder_unlinkToDeath(cb->asBinder().get(), deathRecipient, this /* cookie */);
        if (unlinkToDeathReturnStatus != STATUS_OK) {
            ALOGW("Unable to unlink death recipient: %d", unlinkToDeathReturnStatus);
        }
        AIBinder_DeathRecipient_delete(deathRecipient);
    }

    void Reset() {
        AIBinder_DeathRecipient* deathRecipient = nullptr;
        {
            std::lock_guard<std::mutex> guard(mMutex);
            deathRecipient = clientDeathRecipient_;
            clientDeathRecipient_ = nullptr;
            mCb.reset();
        }
        if (deathRecipient != nullptr) {
            AIBinder_DeathRecipient_delete(deathRecipient);
        }
    }

    void serviceDied() {
        {
            std::lock_guard<std::mutex> guard(mMutex);
            if (mCb == nullptr || AIBinder_isAlive(mCb->asBinder().get())) {
                ALOGE("BluetoothDeathRecipient::serviceDied called but service not dead");
                return;
            }
            ALOGE("Bluetooth remote service has died");
            has_died_ = true;
        }
        mHci->close();
    }

    bool getHasDied() {
        std::lock_guard<std::mutex> guard(mMutex);
        return has_died_;
    }

  private:
    BluetoothHci* mHci;
    std::mutex mMutex;
    std::shared_ptr<IBluetoothHciCallbacks> mCb;
    AIBinder_DeathRecipient* clientDeathRecipient_ = nullptr;
    bool has_died_ = false;
};

void OnDeath(void* cookie) {
    auto* death_recipient = static_cast<BluetoothDeathRecipient*>(cookie);
    death_recipient->serviceDied();
}

BluetoothHci::BluetoothHci() {
    mDeathRecipient = std::make_shared<BluetoothDeathRecipient>(this);
}

ndk::ScopedAStatus BluetoothHci::initialize(const std::shared_ptr<IBluetoothHciCallbacks>& cb) {
    ALOGI("Initializing Bluetooth HCI via AIDL");

    if (cb == nullptr) {
        ALOGE("cb == nullptr! -> Unable to call initializationComplete(ERR)");
        return ndk::ScopedAStatus::fromServiceSpecificError(STATUS_BAD_VALUE);
    }

    std::unique_lock<std::mutex> stateLock(mStateMutex);
    if (mState != HalState::READY) {
        ALOGE("initialize: Unexpected State %d", static_cast<int>(mState));
        stateLock.unlock();
        cb->initializationComplete(Status::ALREADY_INITIALIZED);
        return ndk::ScopedAStatus::ok();
    }

    mState = HalState::INITIALIZING;
    mCb = cb;

    bool rc = VendorInterface::Initialize(
            [cb](bool status) {
                cb->initializationComplete(status ? Status::SUCCESS
                                                  : Status::HARDWARE_INITIALIZATION_ERROR);
            },
            [](const std::vector<uint8_t>&) { LOG_ALWAYS_FATAL("Unexpected command!"); },
            [cb](const std::vector<uint8_t>& raw_acl) { cb->aclDataReceived(raw_acl); },
            [cb](const std::vector<uint8_t>& raw_sco) { cb->scoDataReceived(raw_sco); },
            [cb](const std::vector<uint8_t>& raw_event) { cb->hciEventReceived(raw_event); },
            [cb](const std::vector<uint8_t>& raw_iso) { cb->isoDataReceived(raw_iso); },
            []() { ALOGI("HCI socket device disconnected"); });
    if (!rc) {
        ALOGE("VendorInterface::Initialize failed");
        VendorInterface::Shutdown();
        mDeathRecipient->Reset();
        mCb.reset();
        mState = HalState::READY;
        return ndk::ScopedAStatus::fromServiceSpecificError(STATUS_BAD_VALUE);
    }

    if (!AIBinder_isAlive(cb->asBinder().get()) || !mDeathRecipient->LinkToDeath(cb)) {
        ALOGE("Bluetooth client died before HCI initialization completed");
        VendorInterface::Shutdown();
        mDeathRecipient->Reset();
        mCb.reset();
        mState = HalState::READY;
        return ndk::ScopedAStatus::fromServiceSpecificError(STATUS_BAD_VALUE);
    }

    mState = HalState::ONE_CLIENT;
    stateLock.unlock();

    ALOGI("%s:Bluetooth HCI initialized successfully, state = %d", __func__,
          static_cast<int>(HalState::ONE_CLIENT));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothHci::close() {
    ALOGI("%s:Bluetooth HCI close sequence initiated via AIDL", __func__);
    std::unique_lock<std::mutex> stateLock(mStateMutex);
    if (mState != HalState::ONE_CLIENT && mState != HalState::INITIALIZING) {
        ALOGI("Already closed");
        return ndk::ScopedAStatus::ok();
    }

    mState = HalState::CLOSING;
    ALOGI("%s: HalState set moving to CLOSING", __func__);
    if (!mDeathRecipient->getHasDied()) {
        mDeathRecipient->UnlinkToDeath(mCb);
    } else {
        mDeathRecipient->Reset();
    }
    VendorInterface::Shutdown();
    mDeathRecipient->Reset();
    mCb.reset();
    mState = HalState::READY;
    stateLock.unlock();

    ALOGI("%s: Shutdown complete, HalState moving to READY", __func__);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothHci::sendHciCommand(const std::vector<uint8_t>& packet) {
    return send(PacketType::COMMAND, packet);
}

ndk::ScopedAStatus BluetoothHci::sendAclData(const std::vector<uint8_t>& packet) {
    return send(PacketType::ACL_DATA, packet);
}

ndk::ScopedAStatus BluetoothHci::sendScoData(const std::vector<uint8_t>& packet) {
    return send(PacketType::SCO_DATA, packet);
}

ndk::ScopedAStatus BluetoothHci::sendIsoData(const std::vector<uint8_t>& packet) {
    return send(PacketType::ISO_DATA, packet);
}

ndk::ScopedAStatus BluetoothHci::send(PacketType type, const std::vector<uint8_t>& data) {
    {
        std::lock_guard<std::mutex> guard(mStateMutex);
        if (mState != HalState::ONE_CLIENT) {
            return ndk::ScopedAStatus::fromServiceSpecificError(STATUS_BAD_VALUE);
        }
    }

    auto vendor_interface = VendorInterface::get();
    if (vendor_interface == nullptr) {
        ALOGE("%s: Vendor interface is not available", __func__);
        return ndk::ScopedAStatus::fromServiceSpecificError(STATUS_BAD_VALUE);
    }
    vendor_interface->Send(type, data.data(), data.size());
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::bluetooth::impl
