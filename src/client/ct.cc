/* -*- indent-tabs-mode: nil -*- */
#include <fcntl.h>
#include <fstream>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <iostream>
#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <sstream>
#include <stdio.h>
#include <string>

#include "client/http_log_client.h"
#include "client/log_client.h"
#include "client/ssl_client.h"
#include "log/cert.h"
#include "log/cert_submission_handler.h"
#include "log/ct_extensions.h"
#include "log/log_signer.h"
#include "log/log_verifier.h"
#include "merkletree/merkle_verifier.h"
#include "merkletree/serial_hasher.h"
#include "proto/ct.pb.h"
#include "proto/serializer.h"
#include "util/util.h"

DEFINE_string(ssl_client_trusted_cert_dir, "",
              "Trusted root certificates for the ssl client");
DEFINE_string(ct_server_public_key, "",
              "PEM-encoded public key file of the CT log server");
DEFINE_string(ssl_server, "", "SSL server to connect to");
DEFINE_int32(ssl_server_port, 0, "SSL server port");
DEFINE_string(ct_server_submission, "",
              "Certificate chain to submit to a CT log server. "
              "The file must consist of concatenated PEM certificates.");
DEFINE_string(ct_server, "", "CT log server to connect to");
DEFINE_int32(ct_server_port, 0, "CT log server port (not used for HTTP)");
DEFINE_string(ct_server_response_out, "",
              "Output file for the Signed Certificate Timestamp received from "
              "the CT log server");
DEFINE_bool(precert, false,
            "The submission is a CA precertificate chain");
DEFINE_string(sct_token, "",
              "Input file containing the SCT of the certificate");
DEFINE_string(ssl_client_ct_data_in, "",
              "Input file for reading the SSLClientCTData");
DEFINE_string(ssl_client_ct_data_out, "",
              "Output file for recording the server's leaf certificate, "
              "as well as all received and validated SCTs.");
DEFINE_string(certificate_out, "",
              "Output file for the superfluous certificate");
DEFINE_string(tls_extension_data_out, "", "Output file for TLS extension data");
DEFINE_string(extensions_config_out, "",
              "Output configuration file to append the sct to. Appends the "
              "sct to the end of the file, so the relevant section should be "
              "last in the configuration file.");
DEFINE_bool(ssl_client_require_sct, true, "Fail the SSL handshake if "
            "the server presents no valid SCT token");
DEFINE_bool(ssl_client_expect_handshake_failure, false,
            "Expect the handshake to fail. If this is set to true, then "
            "the program exits with 0 iff there is a handshake failure. "
            "Used for testing.");
DEFINE_string(certificate_chain_in, "", "Certificate chain to analyze, "
              "in PEM format");
DEFINE_string(sct_in, "", "SCT to wrap");
DEFINE_bool(http_log, false, "Use an I-D compliant HTTP log server rather than "
            "the old protocol buffer format");
DEFINE_int32(get_first, 0, "First entry to retrieve with the 'get' command");
DEFINE_int32(get_last, 0, "Last entry to retrieve with the 'get' command");
DEFINE_string(certificate_base, "", "Base name for retrieved certificates - "
              "files will be <base><entry>.<cert>.der");


static const char kUsage[] =
    " <command> ...\n"
    "Known commands:\n"
    "connect - connect to an SSL server\n"
    "upload - upload a submission to a CT log server\n"
    "certificate - make a superfluous proof certificate\n"
    "extension_data - convert an audit proof to TLS extension format\n"
    "configure_proof - write the proof in an X509v3 configuration file\n"
    "diagnose_chain - print info about the SCTs the cert chain carries\n"
    "wrap - take an SCT and certificate chain and wrap them as if they were\n"
    "       retrieved via 'connect'\n"
    "wrap_embedded - take a certificate chain with an embedded SCT and wrap\n"
    "                them as if they were retrieved via 'connect'\n"
    "get_entries - get entries from the log\n"
    "Use --help to display command-line flag options\n";

using ct::LogEntry;
using ct::MerkleAuditProof;
using ct::SignedCertificateTimestamp;
using ct::SignedCertificateTimestampList;
using ct::SSLClientCTData;
using std::string;

using ct::Cert;
using ct::CertChain;

// SCTs presented to clients have to be encoded as a list.
// Helper method for encoding a single SCT.
static string SCTToList(const string &serialized_sct) {
  SignedCertificateTimestampList sct_list;
  sct_list.add_sct_list(serialized_sct);
  string result;
  CHECK_EQ(Serializer::OK, Serializer::SerializeSCTList(sct_list, &result));
  return result;
}

static LogVerifier *GetLogVerifierFromFlags() {
  string log_server_key = FLAGS_ct_server_public_key;
  EVP_PKEY *pkey = NULL;
  FILE *fp = fopen(log_server_key.c_str(), "r");

  PCHECK(fp != static_cast<FILE*>(NULL))
      << "Could not read CT server public key file";
  // No password.
  PEM_read_PUBKEY(fp, &pkey, NULL, NULL);
  CHECK_NE(pkey, static_cast<EVP_PKEY*>(NULL)) <<
      log_server_key << " is not a valid PEM-encoded public key.";
  fclose(fp);

  return new LogVerifier(new LogSigVerifier(pkey),
                         new MerkleVerifier(new Sha256Hasher()));
}

// Adds the data to the cert as an extension, formatted as a single
// ASN.1 octet string.
static void AddOctetExtension(X509 *cert, int nid, const unsigned char *data,
                              int data_len, int critical) {
  // The extension as a single octet string.
  ASN1_OCTET_STRING *inner = ASN1_OCTET_STRING_new();
  CHECK_NOTNULL(inner);
  CHECK_EQ(1, ASN1_OCTET_STRING_set(inner, data, data_len));
  int buf_len = i2d_ASN1_OCTET_STRING(inner, NULL);
  CHECK_GT(buf_len, 0);

  unsigned char *buf = new unsigned char[buf_len];
  unsigned char *p = buf;

  CHECK_EQ(buf_len, i2d_ASN1_OCTET_STRING(inner, &p));

  // The outer, opaque octet string.
  ASN1_OCTET_STRING *asn1_data = ASN1_OCTET_STRING_new();
  CHECK_NOTNULL(asn1_data);
  CHECK_EQ(1, ASN1_OCTET_STRING_set(asn1_data, buf, buf_len));

  X509_EXTENSION *ext = X509_EXTENSION_create_by_NID(NULL, nid, critical,
                                                     asn1_data);
  CHECK_EQ(1, X509_add_ext(cert, ext, -1));

  ASN1_OCTET_STRING_free(inner);
  ASN1_OCTET_STRING_free(asn1_data);
  delete buf;
}

static bool CheckSCT(const SignedCertificateTimestamp &sct,
                     const CertChain &chain, SSLClientCTData *ct_data) {
  SSLClientCTData::SCTInfo *sct_info = ct_data->add_attached_sct_info();
  sct_info->mutable_sct()->CopyFrom(sct);
  LogEntry entry;
  if (!CertSubmissionHandler::X509ChainToEntry(chain, &entry)) {
    LOG(ERROR) << "Failed to reconstruct log entry input from chain";
  } else {
    ct_data->mutable_reconstructed_entry()->CopyFrom(entry);
  }

  LogVerifier *verifier = GetLogVerifierFromFlags();
  string merkle_leaf;
  LogVerifier::VerifyResult result =
      verifier->VerifySignedCertificateTimestamp(ct_data->reconstructed_entry(),
                                                 sct, &merkle_leaf);
  if (result != LogVerifier::VERIFY_OK) {
    LOG(ERROR) << "Verifier returned " << result;
    return false;
  }

  sct_info->set_merkle_leaf_hash(merkle_leaf);

  return true;
}

// Returns true if the server responds with a token; false if
// it responds with an error.
// 0 - ok
// 1 - server says no
// 2 - server unavailable
static int Upload() {
  // Contents should be concatenated PEM entries.
  string contents;
  string submission_file = FLAGS_ct_server_submission;
  PCHECK(util::ReadBinaryFile(submission_file, &contents))
      << "Could not read CT log server submission from " << submission_file;

  LOG(INFO) << "Uploading certificate submission from " << submission_file;
  LOG(INFO) << submission_file << " is " << contents.length() << " bytes.";

  SignedCertificateTimestamp sct;

  if (FLAGS_http_log) {
    HTTPLogClient client(FLAGS_ct_server);
    
    HTTPLogClient::Status ret = client.UploadSubmission(contents, FLAGS_precert,
                                                        &sct);

    if (ret == HTTPLogClient::CONNECT_FAILED) {
      LOG(ERROR) << "Unable to connect";
      return 2;
    }

    if (ret != HTTPLogClient::OK) {
      LOG(ERROR) << "Submission failed, error = " << ret;
      return 1;
    }
  } else {
    LogClient client(FLAGS_ct_server, FLAGS_ct_server_port);
    if (!client.Connect()) {
      LOG(ERROR) << "Unable to connect";
      return 2;
    }

    if (!client.UploadSubmission(contents, FLAGS_precert, &sct)) {
      LOG(ERROR) << "Submission failed";
      return 1;
    }
  }

  /* Temporarily removed: we need the the final chain if the upload
     didn't include it (might be missing the root certificate, might
     be a pre-cert chain). In the meantime this breaks self-tests.

     SSLClientCTData ct_data;
     CertChain chain(contents);
     CHECK(CheckSCT(sct, chain, &ct_data));
  */

  // TODO(ekasper): Process the |contents| bundle so that we can verify
  // the token.

  bool record_response = !FLAGS_ct_server_response_out.empty();
  if (record_response) {
    string response_file = FLAGS_ct_server_response_out;
    std::ofstream token_out(response_file.c_str(),
                            std::ios::out | std::ios::binary);
    PCHECK(token_out.good()) << "Could not open response file " << response_file
                             << " for writing";
    string proof;
    if (Serializer::SerializeSCT(sct, &proof) == Serializer::OK) {
      token_out.write(proof.data(), proof.size());
      LOG(INFO) << "SCT token saved in " << response_file;
      token_out.close();
    } else {
      LOG(ERROR) << "Failed to serialize the server token";
      return 1;
    }
  } else {
    LOG(WARNING) << "No response file specified; SCT token will not be saved.";
  }
  return 0;
}

// FIXME: fix all the memory leaks in this code.
static void MakeCert() {
  string sct;
  PCHECK(util::ReadBinaryFile(FLAGS_sct_token, &sct))
      << "Could not read SCT data from " << FLAGS_sct_token;

  string cert_file = FLAGS_certificate_out;

  int cert_fd = open(cert_file.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0666);
  PCHECK(cert_fd > 0) << "Could not open certificate file " << cert_file
                      << " for writing.";

  BIO *out = BIO_new_fd(cert_fd, BIO_CLOSE);

  X509 *x = X509_new();

  // X509v3 (== 2)
  X509_set_version(x, 2);

  // Random 128 bit serial number
  BIGNUM *serial = BN_new();
  BN_rand(serial, 128, 0, 0);
  BN_to_ASN1_INTEGER(serial, X509_get_serialNumber(x));
  BN_free(serial);

  // Set signature algorithm
  // FIXME: is there an opaque way to get the algorithm structure?
  x->cert_info->signature->algorithm = OBJ_nid2obj(NID_sha1WithRSAEncryption);
  x->cert_info->signature->parameter = NULL;

  // Set the start date to now
  X509_gmtime_adj(X509_get_notBefore(x), 0);
  // End date to now + 1 second
  X509_gmtime_adj(X509_get_notAfter(x), 1);

  // Create the issuer name
  X509_NAME *issuer = X509_NAME_new();
  X509_NAME_add_entry_by_NID(
      issuer, NID_commonName, V_ASN1_PRINTABLESTRING,
      const_cast<unsigned char *>(
          reinterpret_cast<const unsigned char *>("Test")),
      4, 0, -1);
  X509_set_issuer_name(x, issuer);

  // Create the subject name
  X509_NAME *subject = X509_NAME_new();
  X509_NAME_add_entry_by_NID(
      subject, NID_commonName, V_ASN1_PRINTABLESTRING,
      const_cast<unsigned char *>(
          reinterpret_cast<const unsigned char *>("tseT")),
      4, 0, -1);
  X509_set_subject_name(x, subject);

  // Public key
  RSA *rsa = RSA_new();
  static const unsigned char bits[1] = { 3 };
  rsa->n = BN_bin2bn(bits, 1, NULL);
  rsa->e = BN_bin2bn(bits, 1, NULL);
  EVP_PKEY *evp_pkey = EVP_PKEY_new();
  EVP_PKEY_assign_RSA(evp_pkey, rsa);
  X509_PUBKEY_set(&X509_get_X509_PUBKEY(x) , evp_pkey);

  // And finally, the proof in an extension
  string serialized_sct_list = SCTToList(sct);
  AddOctetExtension(x, ct::NID_ctSignedCertificateTimestampList,
                    reinterpret_cast<const unsigned char*>(
                        serialized_sct_list.data()),
                    serialized_sct_list.size(), 1);

  int i = i2d_X509_bio(out, x);
  CHECK_GT(i, 0);

  BIO_free(out);
}

// A sample tool for CAs showing how to add the CT proof as an extension.
// We write the CT proof to the certificate config, so that we can
// sign using the standard openssl signing flow.
// Input:
// (1) an X509v3 configuration file
// (2) A binary proof file.
// Output:
// Append the following line to the end of the file.
// (This means the relevant section should be last in the configuration.)
// 1.2.3.1=DER:[raw encoding of proof]
static void WriteProofToConfig() {
  CHECK(!FLAGS_sct_token.empty()) << google::ProgramUsage();
  CHECK(!FLAGS_extensions_config_out.empty()) << google::ProgramUsage();

  string sct;

  PCHECK(util::ReadBinaryFile(FLAGS_sct_token, &sct))
      << "Could not read SCT data from " << FLAGS_sct_token;

  string serialized_sct_list = SCTToList(sct);

  string conf_file = FLAGS_extensions_config_out;

  std::ofstream conf_out(conf_file.c_str(), std::ios::app);
  PCHECK(conf_out.good()) << "Could not open extensions configuration file "
                          << conf_file << " for writing.";

  conf_out << string(ct::kEmbeddedSCTListOID)
           << "=ASN1:FORMAT:HEX,OCTETSTRING:";

  conf_out << util::HexString(serialized_sct_list) << std::endl;
  conf_out.close();
}

// Wrap the proof in the format expected by the TLS extension,
// so that we can feed it to OpenSSL.
// Currently unused: will be needed once we have an extension number,
// and support in OpenSSL and Apache.
static void ProofToExtensionData() {
  CHECK(!FLAGS_sct_token.empty()) << google::ProgramUsage();
  CHECK(!FLAGS_tls_extension_data_out.empty()) << google::ProgramUsage();

  string serialized_sct;
  PCHECK(util::ReadBinaryFile(FLAGS_sct_token, &serialized_sct))
      << "Could not read SCT data from " << FLAGS_sct_token;
  std::ifstream proof_in(FLAGS_sct_token.c_str(),
                         std::ios::in | std::ios::binary);
  PCHECK(proof_in.good()) << "Could not read SCT data from " << FLAGS_sct_token;

  std::ofstream extension_data_out(FLAGS_tls_extension_data_out.c_str(),
                                   std::ios::out | std::ios::binary);
  PCHECK(extension_data_out.good()) << "Could not open extension data file "
                                    << FLAGS_tls_extension_data_out
                                    << " for writing";

  // Count proof length.
  proof_in.seekg(0, std::ios::end);
  int proof_length = proof_in.tellg();
  // Rewind.
  proof_in.seekg(0, std::ios::beg);

  // Write the length.
  extension_data_out << static_cast<unsigned char>(proof_length >> 8)
                     << static_cast<unsigned char>(proof_length);

  // Now write the proof.
  char *buf = new char[proof_length];
  proof_in.read(buf, proof_length);
  assert(proof_in.gcount() == proof_length);
  extension_data_out.write(buf, proof_length);
  CHECK(!extension_data_out.bad());

  delete[] buf;
  proof_in.close();
  extension_data_out.close();
}

static void WriteSSLClientCTData(const SSLClientCTData &ct_data,
                                 const string &ct_data_out_file) {
  std::ofstream checkpoint_out(ct_data_out_file.c_str(),
                               std::ios::out | std::ios::binary);
  PCHECK(checkpoint_out.good()) << "Could not open checkpoint file "
                                << ct_data_out_file << " for writing";
  string serialized_data;
  CHECK(ct_data.SerializeToString(&serialized_data));
  checkpoint_out << serialized_data;
  checkpoint_out.close();
}

// Return values upon completion
//  0: handshake ok
//  1: handshake error
//  2: connection error
static SSLClient::HandshakeResult Connect() {
  LogVerifier *verifier = GetLogVerifierFromFlags();

  SSLClient client(FLAGS_ssl_server, FLAGS_ssl_server_port,
                   FLAGS_ssl_client_trusted_cert_dir, verifier);

  SSLClient::HandshakeResult result;

  if (FLAGS_ssl_client_require_sct)
    result = client.SSLConnectStrict();
  else
    result = client.SSLConnect();

  if (result == SSLClient::OK) {
    SSLClientCTData ct_data;
    client.GetSSLClientCTData(&ct_data);
    if (ct_data.attached_sct_info_size() > 0) {
      LOG(INFO) << "Received " << ct_data.attached_sct_info_size() << " SCTs";
      VLOG(5) << "Received SCTs:";
      for (int i = 0; i < ct_data.attached_sct_info_size(); ++i)
        VLOG(5) << ct_data.attached_sct_info(i).DebugString();
      if (!FLAGS_ssl_client_ct_data_out.empty())
        WriteSSLClientCTData(ct_data, FLAGS_ssl_client_ct_data_out);
    }
  }
  return result;
}

enum AuditResult {
  // At least one SCT has a valid proof.
  // (Should be unusual to have more than one SCT from the same log,
  // but we audit them all and try to see if any are valid).
  PROOF_OK = 0,
  // No SCTs have valid proofs.
  PROOF_NOT_FOUND = 1,
  CT_SERVER_UNAVAILABLE = 2,
};

static AuditResult Audit() {
  string serialized_data;
  PCHECK(util::ReadBinaryFile(FLAGS_ssl_client_ct_data_in, &serialized_data))
      << "Could not read CT data from " << FLAGS_ssl_client_ct_data_in;
  SSLClientCTData ct_data;
  CHECK(ct_data.ParseFromString(serialized_data))
      << "Failed to parse the stored certificate CT data";
  CHECK(ct_data.has_reconstructed_entry());
  CHECK_GT(ct_data.attached_sct_info_size(), 0);

  LogVerifier *verifier = GetLogVerifierFromFlags();
  string key_id = verifier->KeyID();

  AuditResult audit_result = PROOF_NOT_FOUND;

  for (int i = 0; i < ct_data.attached_sct_info_size(); ++i) {
    LOG(INFO) << "Signed Certificate Timestamp number " << i + 1 << ":\n"
              << ct_data.attached_sct_info(i).sct().DebugString();

    string sct_id = ct_data.attached_sct_info(i).sct().id().key_id();
    if (sct_id != key_id) {
      LOG(WARNING) << "Audit skipped: log server Key ID " << sct_id
                   << " does not match verifier's ID";
      continue;
    }

    MerkleAuditProof proof;

    if (FLAGS_http_log) {
      HTTPLogClient client(FLAGS_ct_server);
      
      LOG(INFO) << "info = "
                << ct_data.attached_sct_info(i).DebugString();
      HTTPLogClient::Status ret = client.QueryAuditProof(
          ct_data.attached_sct_info(i).merkle_leaf_hash(), &proof);

      // HTTP protocol does not supply this.
      proof.mutable_id()->set_key_id(sct_id);

      if (ret == HTTPLogClient::CONNECT_FAILED) {
        LOG(ERROR) << "Unable to connect";
        delete verifier;
        return CT_SERVER_UNAVAILABLE;
      }
      if (ret != HTTPLogClient::OK) {
        LOG(ERROR) << "QueryAuditProof failed, error " << ret;
        continue;
      }
    } else {
      LogClient client(FLAGS_ct_server, FLAGS_ct_server_port);
      if (!client.Connect()) {
        LOG(ERROR) << "Unable to connect";
        delete verifier;
        return CT_SERVER_UNAVAILABLE;
      }

      if (!client.QueryAuditProof(ct_data.attached_sct_info(i)
                                  .merkle_leaf_hash(), &proof)) {
        LOG(INFO) << "Failed to retrieve audit proof";
        continue;
      }
    }

    LOG(INFO) << "Received proof " << proof.DebugString();
    LogVerifier::VerifyResult res =
        verifier->VerifyMerkleAuditProof(ct_data.reconstructed_entry(),
                                         ct_data.attached_sct_info(i).sct(),
                                         proof);
    if (res != LogVerifier::VERIFY_OK) {
      LOG(ERROR) << "Verify error: " << LogVerifier::VerifyResultString(res);
      LOG(ERROR) << "Retrieved Merkle proof is invalid.";
      continue;
    }
    LOG(INFO) << "Proof verified.";
    audit_result = PROOF_OK;
  }
  delete verifier;
  return audit_result;
}

static void DiagnoseCertChain() {
  string cert_file = FLAGS_certificate_chain_in;
  CHECK(!cert_file.empty()) << "Please give a certificate chain with "
                            << "--certificate_chain_in";
  string pem_chain;
  PCHECK(util::ReadBinaryFile(cert_file, &pem_chain))
      << "Could not read certificate chain from " << cert_file;
  CertChain chain(pem_chain);
  CHECK(chain.IsLoaded())
      << cert_file << " is not a valid PEM-encoded certificate chain";


  if (chain.LeafCert()->HasExtension(
          ct::NID_ctEmbeddedSignedCertificateTimestampList) != Cert::TRUE) {
    LOG(ERROR) << "Certificate has no embedded SCTs";
    return;
  }

  LOG(INFO) << "Embedded proof extension found in certificate";

  LogVerifier *verifier = NULL;
  LogEntry entry;
  if (FLAGS_ct_server_public_key.empty()) {
    LOG(WARNING) << "No log server public key given, skipping verification";
  } else {
    verifier = GetLogVerifierFromFlags();
    CertSubmissionHandler::X509ChainToEntry(chain, &entry);
  }

  string serialized_scts;
  if (chain.LeafCert()->OctetStringExtensionData(
          ct::NID_ctEmbeddedSignedCertificateTimestampList,
          &serialized_scts) != Cert::TRUE) {
    LOG(ERROR) << "SCT extension data is invalid.";
    return;
  }

  LOG(INFO) << "Embedded SCT extension length is " << serialized_scts.length()
            << " bytes";

  SignedCertificateTimestampList sct_list;
  if (Deserializer::DeserializeSCTList(serialized_scts, &sct_list) !=
      Deserializer::OK) {
    LOG(ERROR) << "Failed to parse SCT list from certificate";
    return;
  }

  LOG(INFO) << "Certificate has " << sct_list.sct_list_size() << " SCTs";
  for (int i = 0; i < sct_list.sct_list_size(); ++i) {
    SignedCertificateTimestamp sct;
    if (Deserializer::DeserializeSCT(sct_list.sct_list(i), &sct) !=
        Deserializer::OK) {
      LOG(ERROR) << "Failed to parse SCT number " << i + 1;
      continue;
    }
    LOG(INFO) << "SCT number " << i + 1 << ":\n" << sct.DebugString();
    if (verifier != NULL) {
      if (sct.id().key_id() != verifier->KeyID()) {
        LOG(WARNING) << "SCT key ID does not match verifier's ID, skipping";
        continue;
      } else {
        LogVerifier::VerifyResult res =
            verifier->VerifySignedCertificateTimestamp(entry, sct);
        if (res == LogVerifier::VERIFY_OK)
          LOG(INFO) << "SCT verified";
        else
          LOG(ERROR) << "SCT verification failed: "
                     << LogVerifier::VerifyResultString(res);
      }
    }
  }
}

// Wrap an SCT in an SSLClientCTData as if it came from an SSL server.
void Wrap() {
  string serialized_data;
  PCHECK(util::ReadBinaryFile(FLAGS_sct_in, &serialized_data))
      << "Could not read SCT data from " << FLAGS_sct_in;
  SignedCertificateTimestamp sct;
  CHECK_EQ(Deserializer::DeserializeSCT(serialized_data, &sct),
           Deserializer::OK);

  // FIXME(benl): This code is shared with DiagnoseCertChain().
  string cert_file = FLAGS_certificate_chain_in;
  CHECK(!cert_file.empty()) << "Please give a certificate chain with "
                            << "--certificate_chain_in";
  string pem_chain;
  PCHECK(util::ReadBinaryFile(cert_file, &pem_chain))
      << "Could not read certificate chain from " << cert_file;
  CertChain chain(pem_chain);
  CHECK(chain.IsLoaded())
      << cert_file << " is not a valid PEM-encoded certificate chain";

  SSLClientCTData ct_data;
  CHECK(CheckSCT(sct, chain, &ct_data));

  WriteSSLClientCTData(ct_data, FLAGS_ssl_client_ct_data_out);
}

// Wrap an embedded SCT in an SSLClientCTData as if it came from an SSL server.
void WrapEmbedded() {
  // FIXME(benl): This code is shared with DiagnoseCertChain().
  string cert_file = FLAGS_certificate_chain_in;
  CHECK(!cert_file.empty()) << "Please give a certificate chain with "
                            << "--certificate_chain_in";
  string pem_chain;
  PCHECK(util::ReadBinaryFile(cert_file, &pem_chain))
      << "Could not read certificate chain from " << cert_file;
  CertChain chain(pem_chain);
  CHECK(chain.IsLoaded())
      << cert_file << " is not a valid PEM-encoded certificate chain";
  CHECK_EQ(Cert::TRUE, chain.LeafCert()->HasExtension(
               ct::NID_ctEmbeddedSignedCertificateTimestampList));

  string serialized_scts;
  CHECK_EQ(Cert::TRUE, chain.LeafCert()->OctetStringExtensionData(
               ct::NID_ctEmbeddedSignedCertificateTimestampList,
               &serialized_scts));
  SignedCertificateTimestampList sct_list;
  CHECK_EQ(Deserializer::OK,
           Deserializer::DeserializeSCTList(serialized_scts, &sct_list));

  // FIXME(benl): handle multiple SCTs!
  CHECK_EQ(1, sct_list.sct_list().size());

  SignedCertificateTimestamp sct;
  CHECK_EQ(Deserializer::DeserializeSCT(sct_list.sct_list(0), &sct),
           Deserializer::OK);

  SSLClientCTData ct_data;
  CHECK(CheckSCT(sct, chain, &ct_data));

  WriteSSLClientCTData(ct_data, FLAGS_ssl_client_ct_data_out);
}

static void WriteCertificate(const std::string &cert, int entry,
                             int cert_number) {
 std::ostringstream outname;
 outname << FLAGS_certificate_base << entry << '.' << cert_number << ".der";
 std::ofstream out(outname.str().c_str(),
                   std::ios::binary | std::ios::trunc);
 CHECK(out.good());
 out << cert;
}

void GetEntries() {
  CHECK(FLAGS_http_log);

  HTTPLogClient client(FLAGS_ct_server);
  std::vector<HTTPLogClient::LogEntry> entries;
  HTTPLogClient::Status error = client.GetEntries(FLAGS_get_first,
                                                  FLAGS_get_last, &entries);
  CHECK(error == HTTPLogClient::OK);

  CHECK(!FLAGS_certificate_base.empty());

  int e = FLAGS_get_first;
  for (std::vector<HTTPLogClient::LogEntry>::const_iterator entry =
           entries.begin(); entry != entries.end(); ++entry, ++e) {
    if (entry->leaf.timestamped_entry().entry_type() == ct::X509_ENTRY) {
      WriteCertificate(entry->leaf.timestamped_entry().signed_entry().x509(),
                       e, 0);
      const ct::X509ChainEntry &x509chain = entry->entry.x509_entry();
      for (int n = 0; n < x509chain.certificate_chain_size(); ++n)
        WriteCertificate(x509chain.certificate_chain(n), e, n + 1);
    }
    // TODO(benl): precerts
  }
}

// Exit code upon normal exit:
// 0: success
// 1: failure
// - for log server: connection failed or the server replied with an error
// - for SSL server: connection failed, handshake failed when success was
//                   expected or vice versa
// 2: initial connection to the (log/ssl) server failed
// Exit code upon abnormal exit (CHECK failures): != 0
// (on UNIX, 134 is expected)
int main(int argc, char **argv) {
  google::SetUsageMessage(argv[0] + string(kUsage));
  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

  const string main_command(argv[0]);
  if (argc < 2) {
    std::cout << google::ProgramUsage();
    return 1;
  }

  SSL_library_init();
  ERR_load_SSL_strings();
  ct::LoadCtExtensions();

  const string cmd(argv[1]);

  int ret = 0;
  if (cmd == "connect") {
    bool want_fail = FLAGS_ssl_client_expect_handshake_failure;
    SSLClient::HandshakeResult result = Connect();
    if ((!want_fail && result != SSLClient::OK) ||
        (want_fail && result != SSLClient::HANDSHAKE_FAILED))
      ret = 1;
  } else  if (cmd == "upload") {
    ret = Upload();
  } else if (cmd == "audit") {
    ret = Audit();
  } else if (cmd == "certificate") {
    MakeCert();
  } else if (cmd == "extension_data") {
    ProofToExtensionData();
  } else if (cmd == "configure_proof") {
    WriteProofToConfig();
  } else if (cmd == "diagnose_chain") {
    DiagnoseCertChain();
  } else if (cmd == "wrap") {
    Wrap();
  } else if (cmd == "wrap_embedded") {
    WrapEmbedded();
  } else if (cmd == "get_entries") {
    GetEntries();
  } else {
    std::cout << google::ProgramUsage();
  }

  return ret;
}
