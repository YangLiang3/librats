/* Copyright (c) 2022 Intel Corporation
 * Copyright (c) 2020-2021 Alibaba Cloud
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// clang-format off
#include <string.h>
#include <unistd.h>
#include <librats/log.h>
#include <librats/attester.h>
#include <stddef.h>
#include <sgx_uae_service.h>
#include <sgx_quote_3.h>
#include <sgx_ql_quote.h>
#ifndef SGX
#include <sgx_dcap_quoteverify.h>
#include <sgx_dcap_ql_wrapper.h>
#endif
#include <sgx_ql_lib_common.h>
#include <assert.h>
#include <sgx_error.h>
#include <sgx_utils.h>
#include "sgx_ecdsa.h"
#ifdef SGX
#include "rats_t.h"

sgx_status_t sgx_generate_evidence(const uint8_t *hash, sgx_report_t *app_report)
{
	sgx_report_data_t report_data;
	assert(sizeof(report_data.d) >= SHA256_HASH_SIZE);
	memset(&report_data, 0, sizeof(sgx_report_data_t));
	memcpy(report_data.d, hash, SHA256_HASH_SIZE);

	sgx_target_info_t qe_target_info;
	memset(&qe_target_info, 0, sizeof(sgx_target_info_t));
	sgx_status_t sgx_error = ocall_get_target_info(&qe_target_info);
	if (SGX_SUCCESS != sgx_error)
		return sgx_error;

	/* Generate the report for the app_rats */
	sgx_error = sgx_create_report(&qe_target_info, &report_data, app_report);
	return sgx_error;
}
#elif defined(OCCLUM)
#include "sgx_edger8r.h"
#include "sgx_report.h"
#include "quote_generation.h"

int generate_quote(int sgx_fd, sgxioc_gen_dcap_quote_arg_t *gen_quote_arg)
{
	if (gen_quote_arg == NULL) {
		RATS_ERR("NULL gen_quote_arg\n");
		return -1;
	}

	if (ioctl(sgx_fd, SGXIOC_GEN_DCAP_QUOTE, gen_quote_arg) < 0) {
		RATS_ERR("failed to ioctl get quote\n");
		return -1;
	}

	return 0;
}
#endif
// clang-format on

rats_attester_err_t sgx_ecdsa_collect_evidence(rats_attester_ctx_t *ctx,
					       attestation_evidence_t *evidence,
					       const uint8_t *hash,
					       __attribute__((unused)) uint32_t hash_len)
{
	RATS_DEBUG("ctx %p, evidence %p, hash %p\n", ctx, evidence, hash);

#ifdef OCCLUM
	int sgx_fd;
	if ((sgx_fd = open("/dev/sgx", O_RDONLY)) < 0) {
		RATS_ERR("failed to open /dev/sgx\n");
		return RATS_ATTESTER_ERR_INVALID;
	}

	sgx_report_data_t report_data = {
		0,
	};
	assert(sizeof(report_data.d) > SHA256_HASH_SIZE);
	memcpy(report_data.d, hash, SHA256_HASH_SIZE);

	uint32_t quote_size = 0;
	if (ioctl(sgx_fd, SGXIOC_GET_DCAP_QUOTE_SIZE, &quote_size) < 0) {
		RATS_ERR("failed to ioctl get quote size\n");
		return RATS_ATTESTER_ERR_INVALID;
	}

	sgxioc_gen_dcap_quote_arg_t gen_quote_arg = { .report_data = &report_data,
						      .quote_len = &quote_size,
						      .quote_buf = evidence->ecdsa.quote };

	if (generate_quote(sgx_fd, &gen_quote_arg) != 0) {
		RATS_ERR("failed to generate quote\n");
		close(sgx_fd);
		return RATS_ATTESTER_ERR_INVALID;
	}
#else

	sgx_report_t app_report;
	sgx_status_t status = sgx_generate_evidence(hash, &app_report);
	if (status != SGX_SUCCESS) {
		RATS_ERR("failed to generate evidence %#x\n", status);
		return SGX_ECDSA_ATTESTER_ERR_CODE((int)status);
	}

	rats_attester_err_t qe3_ret;
	int sgx_status;
	uint32_t quote_size = 0;
	sgx_status = ocall_qe_get_quote_size(&qe3_ret, &quote_size);
	if (SGX_SUCCESS != sgx_status || RATS_ATTESTER_ERR_NONE != qe3_ret) {
		RATS_ERR("sgx_qe_get_quote_size(): 0x%04x, 0x%04x\n", sgx_status, qe3_ret);
		return SGX_ECDSA_ATTESTER_ERR_CODE((int)qe3_ret);
	}

	sgx_status = ocall_qe_get_quote(&qe3_ret, &app_report, quote_size, evidence->ecdsa.quote);
	if (SGX_SUCCESS != sgx_status || RATS_ATTESTER_ERR_NONE != qe3_ret) {
		RATS_ERR("sgx_qe_get_quote(): 0x%04x, 0x%04x\n", sgx_status, qe3_ret);
		return SGX_ECDSA_ATTESTER_ERR_CODE((int)qe3_ret);
	}
#endif

	RATS_DEBUG("Succeed to generate the quote!\n");

	/* Essentially speaking, sgx_ecdsa_qve verifier generates the same
	 * format of quote as sgx_ecdsa.
	 */
	snprintf(evidence->type, sizeof(evidence->type), "%s", "sgx_ecdsa");
	evidence->ecdsa.quote_len = quote_size;

	return RATS_ATTESTER_ERR_NONE;
}
