/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/U2FHIDTokenManager.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "mozilla/StaticMutex.h"

namespace mozilla {
namespace dom {

static StaticMutex gInstanceMutex;
static U2FHIDTokenManager* gInstance;
static nsIThread* gPBackgroundThread;

static void
u2f_register_callback(uint64_t aTransactionId, rust_u2f_result* aResult)
{
  UniquePtr<U2FResult> rv = MakeUnique<U2FResult>(aTransactionId, aResult);

  StaticMutexAutoLock lock(gInstanceMutex);
  if (!gInstance || NS_WARN_IF(!gPBackgroundThread)) {
    return;
  }

  nsCOMPtr<nsIRunnable> r(NewRunnableMethod<UniquePtr<U2FResult>&&>(
      "U2FHIDTokenManager::HandleRegisterResult", gInstance,
      &U2FHIDTokenManager::HandleRegisterResult, Move(rv)));

  MOZ_ALWAYS_SUCCEEDS(gPBackgroundThread->Dispatch(r.forget(),
                                                   NS_DISPATCH_NORMAL));
}

static void
u2f_sign_callback(uint64_t aTransactionId, rust_u2f_result* aResult)
{
  UniquePtr<U2FResult> rv = MakeUnique<U2FResult>(aTransactionId, aResult);

  StaticMutexAutoLock lock(gInstanceMutex);
  if (!gInstance || NS_WARN_IF(!gPBackgroundThread)) {
    return;
  }

  nsCOMPtr<nsIRunnable> r(NewRunnableMethod<UniquePtr<U2FResult>&&>(
      "U2FHIDTokenManager::HandleSignResult", gInstance,
      &U2FHIDTokenManager::HandleSignResult, Move(rv)));

  MOZ_ALWAYS_SUCCEEDS(gPBackgroundThread->Dispatch(r.forget(),
                                                   NS_DISPATCH_NORMAL));
}

U2FHIDTokenManager::U2FHIDTokenManager() : mTransactionId(0)
{
  StaticMutexAutoLock lock(gInstanceMutex);
  mozilla::ipc::AssertIsOnBackgroundThread();
  MOZ_ASSERT(XRE_IsParentProcess());
  MOZ_ASSERT(!gInstance);

  mU2FManager = rust_u2f_mgr_new();
  gPBackgroundThread = NS_GetCurrentThread();
  MOZ_ASSERT(gPBackgroundThread, "This should never be null!");
  gInstance = this;
}

void
U2FHIDTokenManager::Drop()
{
  {
    StaticMutexAutoLock lock(gInstanceMutex);
    mozilla::ipc::AssertIsOnBackgroundThread();

    mRegisterPromise.RejectIfExists(NS_ERROR_DOM_UNKNOWN_ERR, __func__);
    mSignPromise.RejectIfExists(NS_ERROR_DOM_UNKNOWN_ERR, __func__);

    gInstance = nullptr;
  }

  // Release gInstanceMutex before we call U2FManager::drop(). It will wait
  // for the work queue thread to join, and that requires the
  // u2f_{register,sign}_callback to lock and return.
  rust_u2f_mgr_free(mU2FManager);
  mU2FManager = nullptr;

  // Reset transaction ID so that queued runnables exit early.
  mTransactionId = 0;
}

// A U2F Register operation causes a new key pair to be generated by the token.
// The token then returns the public key of the key pair, and a handle to the
// private key, which is a fancy way of saying "key wrapped private key", as
// well as the generated attestation certificate and a signature using that
// certificate's private key.
//
// The KeyHandleFromPrivateKey and PrivateKeyFromKeyHandle methods perform
// the actual key wrap/unwrap operations.
//
// The format of the return registration data is as follows:
//
// Bytes  Value
// 1      0x05
// 65     public key
// 1      key handle length
// *      key handle
// ASN.1  attestation certificate
// *      attestation signature
//
RefPtr<U2FRegisterPromise>
U2FHIDTokenManager::Register(const WebAuthnMakeCredentialInfo& aInfo)
{
  mozilla::ipc::AssertIsOnBackgroundThread();

  uint64_t registerFlags = 0;

  const WebAuthnAuthenticatorSelection& sel = aInfo.AuthenticatorSelection();

  // Set flags for credential creation.
  if (sel.requireResidentKey()) {
    registerFlags |= U2F_FLAG_REQUIRE_RESIDENT_KEY;
  }
  if (sel.requireUserVerification()) {
    registerFlags |= U2F_FLAG_REQUIRE_USER_VERIFICATION;
  }
  if (sel.requirePlatformAttachment()) {
    registerFlags |= U2F_FLAG_REQUIRE_PLATFORM_ATTACHMENT;
  }

  ClearPromises();
  mCurrentAppId = aInfo.RpIdHash();
  mTransactionId = rust_u2f_mgr_register(mU2FManager,
                                         registerFlags,
                                         (uint64_t)aInfo.TimeoutMS(),
                                         u2f_register_callback,
                                         aInfo.ClientDataHash().Elements(),
                                         aInfo.ClientDataHash().Length(),
                                         aInfo.RpIdHash().Elements(),
                                         aInfo.RpIdHash().Length(),
                                         U2FKeyHandles(aInfo.ExcludeList()).Get());

  if (mTransactionId == 0) {
    return U2FRegisterPromise::CreateAndReject(NS_ERROR_DOM_UNKNOWN_ERR, __func__);
  }

  return mRegisterPromise.Ensure(__func__);
}

// A U2F Sign operation creates a signature over the "param" arguments (plus
// some other stuff) using the private key indicated in the key handle argument.
//
// The format of the signed data is as follows:
//
//  32    Application parameter
//  1     User presence (0x01)
//  4     Counter
//  32    Challenge parameter
//
// The format of the signature data is as follows:
//
//  1     User presence
//  4     Counter
//  *     Signature
//
RefPtr<U2FSignPromise>
U2FHIDTokenManager::Sign(const WebAuthnGetAssertionInfo& aInfo)
{
  mozilla::ipc::AssertIsOnBackgroundThread();

  uint64_t signFlags = 0;

  // Set flags for credential requests.
  if (aInfo.RequireUserVerification()) {
    signFlags |= U2F_FLAG_REQUIRE_USER_VERIFICATION;
  }

  mCurrentAppId = aInfo.RpIdHash();
  nsTArray<nsTArray<uint8_t>> appIds;
  appIds.AppendElement(mCurrentAppId);

  // Process extensions.
  for (const WebAuthnExtension& ext: aInfo.Extensions()) {
    if (ext.type() == WebAuthnExtension::TWebAuthnExtensionAppId) {
      appIds.AppendElement(ext.get_WebAuthnExtensionAppId().AppId());
    }
  }

  ClearPromises();
  mTransactionId = rust_u2f_mgr_sign(mU2FManager,
                                     signFlags,
                                     (uint64_t)aInfo.TimeoutMS(),
                                     u2f_sign_callback,
                                     aInfo.ClientDataHash().Elements(),
                                     aInfo.ClientDataHash().Length(),
                                     U2FAppIds(appIds).Get(),
                                     U2FKeyHandles(aInfo.AllowList()).Get());
  if (mTransactionId == 0) {
    return U2FSignPromise::CreateAndReject(NS_ERROR_DOM_UNKNOWN_ERR, __func__);
  }

  return mSignPromise.Ensure(__func__);
}

void
U2FHIDTokenManager::Cancel()
{
  mozilla::ipc::AssertIsOnBackgroundThread();

  ClearPromises();
  mTransactionId = rust_u2f_mgr_cancel(mU2FManager);
}

void
U2FHIDTokenManager::HandleRegisterResult(UniquePtr<U2FResult>&& aResult)
{
  mozilla::ipc::AssertIsOnBackgroundThread();

  if (aResult->GetTransactionId() != mTransactionId) {
    return;
  }

  MOZ_ASSERT(!mRegisterPromise.IsEmpty());

  if (aResult->IsError()) {
    mRegisterPromise.Reject(aResult->GetError(), __func__);
    return;
  }

  nsTArray<uint8_t> registration;
  if (!aResult->CopyRegistration(registration)) {
    mRegisterPromise.Reject(NS_ERROR_DOM_UNKNOWN_ERR, __func__);
    return;
  }

  // Will be set by the U2FTokenManager.
  bool directAttestationPermitted = false;
  WebAuthnMakeCredentialResult result(registration, directAttestationPermitted);
  mRegisterPromise.Resolve(Move(result), __func__);
}

void
U2FHIDTokenManager::HandleSignResult(UniquePtr<U2FResult>&& aResult)
{
  mozilla::ipc::AssertIsOnBackgroundThread();

  if (aResult->GetTransactionId() != mTransactionId) {
    return;
  }

  MOZ_ASSERT(!mSignPromise.IsEmpty());

  if (aResult->IsError()) {
    mSignPromise.Reject(aResult->GetError(), __func__);
    return;
  }

  nsTArray<uint8_t> appId;
  if (!aResult->CopyAppId(appId)) {
    mSignPromise.Reject(NS_ERROR_DOM_UNKNOWN_ERR, __func__);
    return;
  }

  nsTArray<uint8_t> keyHandle;
  if (!aResult->CopyKeyHandle(keyHandle)) {
    mSignPromise.Reject(NS_ERROR_DOM_UNKNOWN_ERR, __func__);
    return;
  }

  nsTArray<uint8_t> signature;
  if (!aResult->CopySignature(signature)) {
    mSignPromise.Reject(NS_ERROR_DOM_UNKNOWN_ERR, __func__);
    return;
  }

  nsTArray<WebAuthnExtensionResult> extensions;

  if (appId != mCurrentAppId) {
    // Indicate to the RP that we used the FIDO appId.
    extensions.AppendElement(WebAuthnExtensionResultAppId(true));
  }

  WebAuthnGetAssertionResult result(appId, keyHandle, signature, extensions);
  mSignPromise.Resolve(Move(result), __func__);
}

}
}