#include <glog/logging.h>
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "client/client.h"
#include "client/ssl_client.h"
#include "log/cert.h"
#include "log/cert_submission_handler.h"
#include "log/ct_extensions.h"
#include "log/log_verifier.h"
#include "merkletree/serial_hasher.h"
#include "proto/serializer.h"

using ct::SignedCertificateTimestamp;
using ct::SignedCertificateTimestampList;
using ct::SSLClientCTData;
using ct::LogEntry;
using std::string;

using ct::Cert;
using ct::CertChain;

// TODO(ekasper): handle Cert::Status errors.
SSLClient::SSLClient(const string &server, uint16_t port,
                     const string &ca_dir, LogVerifier *verifier)
    : client_(server, port),
      ctx_(NULL),
      verify_args_(verifier),
      connected_(false) {
  ctx_ = SSL_CTX_new(TLSv1_client_method());
  CHECK_NOTNULL(ctx_);

  // SSL_VERIFY_PEER makes the connection abort immediately
  // if verification fails.
  SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, NULL);
  // Set trusted CA certs.
  if (!ca_dir.empty()) {
    CHECK_EQ(1, SSL_CTX_load_verify_locations(ctx_, NULL, ca_dir.c_str()))
        << "Unable to load trusted CA certificates.";
  } else {
    LOG(WARNING) << "No trusted CA certificates given.";
  }

  // The verify callback gets called before the audit proof callback.
  SSL_CTX_set_cert_verify_callback(ctx_, &VerifyCallback, &verify_args_);
}

SSLClient::~SSLClient() {
  Disconnect();
  if (ctx_ != NULL)
    SSL_CTX_free(ctx_);
  delete verify_args_.verifier;
}

bool SSLClient::Connected() const {
  return connected_;
}

void SSLClient::Disconnect() {
  if (ssl_ != NULL) {
    SSL_shutdown(ssl_);
    SSL_free(ssl_);
    ssl_ = NULL;
    LOG(INFO) << "SSL session finished";
  }
  client_.Disconnect();
  connected_ = false;
}

void SSLClient::GetSSLClientCTData(SSLClientCTData *data) const {
  CHECK(Connected());
  data->CopyFrom(verify_args_.ct_data);
}

// FIXME(ekasper): This code assumes in several places that a certificate has
// *either* embedded proofs *or* regular proofs in a superfluous certificate
// *or* regular proofs in a TLS extension but not several at the same time.
// It's of course for example entirely possible that a cert with an embedded
// proof is re-submitted (or submitted to another log) and the server attaches
// that proof too, but let's not complicate things for now.
// static
LogVerifier::VerifyResult
SSLClient::VerifySCT(const string &token, LogVerifier *verifier,
                     SSLClientCTData *data) {
  CHECK(data->has_reconstructed_entry());
  SignedCertificateTimestamp local_sct;
  // Skip over bad SCTs. These could be either badly encoded ones, or SCTs whose
  // version we don't understand.
  if (Deserializer::DeserializeSCT(token, &local_sct) != Deserializer::OK)
    return LogVerifier::INVALID_FORMAT;

  string merkle_leaf;
  LogVerifier::VerifyResult result =
      verifier->VerifySignedCertificateTimestamp(data->reconstructed_entry(),
                                                 local_sct, &merkle_leaf);
  if (result != LogVerifier::VERIFY_OK)
    return result;
  SSLClientCTData::SCTInfo *sct_info = data->add_attached_sct_info();
  sct_info->set_merkle_leaf_hash(merkle_leaf);
  sct_info->mutable_sct()->CopyFrom(local_sct);
  return LogVerifier::VERIFY_OK;
}

// static
int SSLClient::VerifyCallback(X509_STORE_CTX *ctx, void *arg) {
  VerifyCallbackArgs *args = reinterpret_cast<VerifyCallbackArgs*>(arg);
  CHECK_NOTNULL(args);
  LogVerifier *verifier = args->verifier;
  CHECK_NOTNULL(verifier);

  int vfy = X509_verify_cert(ctx);
  if (vfy != 1) {
    LOG(ERROR) << "Certificate verification failed.";
    return vfy;
  }

  // If verify passed then surely we must have a cert.
  CHECK_NOTNULL(ctx->cert);

  CertChain chain, input_chain;
  // ctx->untrusted is the input chain.
  // ctx->chain is the chain of X509s that OpenSSL constructed and verified.
  CHECK_NOTNULL(ctx->chain);
  int chain_size = sk_X509_num(ctx->chain);
  // Should contain at least the leaf.
  CHECK_GE(chain_size, 1);
  for (int i = 0; i < chain_size; ++i)
    chain.AddCert(new Cert(X509_dup(sk_X509_value(ctx->chain, i))));

  CHECK_NOTNULL(ctx->untrusted);
  chain_size = sk_X509_num(ctx->untrusted);
  // Should contain at least the leaf.
  CHECK_GE(chain_size, 1);
  for (int i = 0; i < chain_size; ++i)
    input_chain.AddCert(new Cert(X509_dup(sk_X509_value(ctx->untrusted, i))));

  string serialized_scts;
  // First, see if the cert has an embedded proof.
  if (chain.LeafCert()->HasExtension(
          ct::NID_ctEmbeddedSignedCertificateTimestampList) == Cert::TRUE) {
        LOG(INFO) << "Embedded proof extension found in certificate, "
                  << "verifying...";
        Cert::Status status = chain.LeafCert()->OctetStringExtensionData(
               ct::NID_ctEmbeddedSignedCertificateTimestampList,
               &serialized_scts);
        if (status != Cert::TRUE) {
          // Any error here is likely OpenSSL acting up, so just die.
          CHECK_EQ(Cert::FALSE, status);
          LOG(ERROR) << "Failed to parse extension data: corrupt cert?";
        }
        // Else look for the proof in a superfluous cert.
        // Let's assume the superfluous cert is always last in the chain.
  } else if (input_chain.Length() > 1 && input_chain.LastCert()->HasExtension(
      ct::NID_ctSignedCertificateTimestampList) == Cert::TRUE) {
    LOG(INFO) << "Proof extension found in certificate, verifying...";
    Cert::Status status = input_chain.LastCert()->OctetStringExtensionData(
        ct::NID_ctSignedCertificateTimestampList, &serialized_scts);
    if (status != Cert::TRUE) {
      // Any error here is likely OpenSSL acting up, so just die.
      CHECK_EQ(Cert::FALSE, status);
      LOG(ERROR) << "Failed to parse extension data: corrupt cert?";
    }
  }

  if (!serialized_scts.empty()) {
    LogEntry entry;
    if (!CertSubmissionHandler::X509ChainToEntry(chain, &entry)) {
      LOG(ERROR) << "Failed to reconstruct log entry input from chain";
    } else {
      args->ct_data.mutable_reconstructed_entry()->CopyFrom(entry);
      args->ct_data.set_certificate_sha256_hash(
          Sha256Hasher::Sha256Digest(Serializer::LeafCertificate(entry)));
      // Only writes the checkpoint if verification succeeds.
      // Note: an optimized client could only verify the signature if it's
      // a certificate it hasn't seen before.
      SignedCertificateTimestampList sct_list;
      if (Deserializer::DeserializeSCTList(serialized_scts, &sct_list) !=
          Deserializer::OK) {
        LOG(ERROR) << "Failed to parse SCT list.";
      } else {
        LOG(INFO) << "Received " << sct_list.sct_list_size() << " SCTs";
        for (int i = 0; i < sct_list.sct_list_size(); ++i) {
          LogVerifier::VerifyResult result =
              VerifySCT(sct_list.sct_list(i), verifier, &args->ct_data);

          if (result == LogVerifier::VERIFY_OK) {
            LOG(INFO) << "SCT number " << i + 1 << " verified";
            args->sct_verified = true;
          } else {
            LOG(ERROR) << "Verification for SCT number " << i + 1 << " failed: "
                       << LogVerifier::VerifyResultString(result);
          }
        } // end for
      }
    }
  }  // end if (!serialized_scts.empty())

  if (!args->sct_verified && args->require_sct) {
    LOG(ERROR) << "No valid SCT found";
    return 0;
  }

  return 1;
}

void SSLClient::ResetVerifyCallbackArgs(bool strict) {
  verify_args_.sct_verified = false;
  verify_args_.require_sct = strict;
  verify_args_.ct_data.CopyFrom(SSLClientCTData::default_instance());
}

SSLClient::HandshakeResult SSLClient::SSLConnect(bool strict) {
  if (!client_.Connect())
    return SERVER_UNAVAILABLE;

  ssl_ = SSL_new(ctx_);
  CHECK_NOTNULL(ssl_);
  BIO *bio = BIO_new_socket(client_.fd(), BIO_NOCLOSE);
  CHECK_NOTNULL(bio);
  // Takes ownership of bio.
  SSL_set_bio(ssl_, bio, bio);

  ResetVerifyCallbackArgs(strict);
  int ret = SSL_connect(ssl_);
  HandshakeResult result;
  if (ret == 1) {
    LOG(INFO) << "Handshake successful. SSL session started";
    connected_ = true;
    DCHECK(!verify_args_.require_sct || verify_args_.sct_verified);
    result = OK;
  } else {
    // TODO(ekasper): look into OpenSSL error stack to determine
    // the error reason. Could be unrelated to SCT verification.
    LOG(ERROR) << "SSL handshake failed";
    result = HANDSHAKE_FAILED;
    Disconnect();
  }
  return result;
}
