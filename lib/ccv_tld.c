#include "ccv.h"
#include "ccv_internal.h"
#include "3rdparty/sfmt/SFMT.h"

#define TLD_GRID_SPARSITY (10)
#define TLD_PATCH_SIZE (10)

static CCV_IMPLEMENT_MEDIAN(_ccv_tld_median, float)

static float _ccv_tld_norm_cross_correlate(ccv_dense_matrix_t* r0, ccv_dense_matrix_t* r1)
{
	assert(CCV_GET_CHANNEL(r0->type) == CCV_C1 && CCV_GET_DATA_TYPE(r0->type) == CCV_8U);
	assert(CCV_GET_CHANNEL(r1->type) == CCV_C1 && CCV_GET_DATA_TYPE(r1->type) == CCV_8U);
	assert(r0->rows == r1->rows && r0->cols == r1->cols);
	int x, y;
	int sum0 = 0, sum1 = 0;
	unsigned char* r0_ptr = r0->data.u8;
	unsigned char* r1_ptr = r1->data.u8;
	for (y = 0; y < r0->rows; y++)
	{
		for (x = 0; x < r0->cols; x++)
		{
			sum0 += r0_ptr[x];
			sum1 += r1_ptr[x];
		}
		r0_ptr += r0->step;
		r1_ptr += r1->step;
	}
	r0_ptr = r0->data.u8;
	r1_ptr = r1->data.u8;
	float mr0 = (float)sum0 / (TLD_PATCH_SIZE * TLD_PATCH_SIZE);
	float mr1 = (float)sum1 / (TLD_PATCH_SIZE * TLD_PATCH_SIZE);
	float r0r1 = 0, r0r0 = 0, r1r1 = 0;
	for (y = 0; y < r0->rows; y++)
	{
		for (x = 0; x < r0->cols; x++)
		{
			float r0f = r0_ptr[x] - mr0;
			float r1f = r1_ptr[x] - mr1;
			r0r1 += r0f * r1f;
			r0r0 += r0f * r0f;
			r1r1 += r1f * r1f;
		}
		r0_ptr += r0->step;
		r1_ptr += r1->step;
	}
	return r0r1 / sqrtf(r0r0 * r1r1);
}

static ccv_rect_t _ccv_tld_short_term_track(ccv_dense_matrix_t* a, ccv_dense_matrix_t* b, ccv_rect_t box, ccv_tld_param_t params)
{
	ccv_rect_t newbox = ccv_rect(0, 0, 0, 0);
	ccv_array_t* point_a = ccv_array_new(sizeof(ccv_decimal_point_t), (TLD_GRID_SPARSITY - 1) * (TLD_GRID_SPARSITY - 1), 0);
	float gapx = (float)box.width / TLD_GRID_SPARSITY;
	float gapy = (float)box.height / TLD_GRID_SPARSITY;
	for (float x = gapx * 0.5; x < box.width; x += gapx)
		for (float y = gapy * 0.5; y < box.height; y += gapy)
		{
			ccv_decimal_point_t point = ccv_decimal_point(box.x + x, box.y + y);
			ccv_array_push(point_a, &point);
		}
	ccv_array_t* point_b = 0;
	ccv_optical_flow_lucas_kanade(a, b, point_a, &point_b, params.win_size, params.level, params.min_eigen);
	ccv_array_t* point_c = 0;
	ccv_optical_flow_lucas_kanade(b, a, point_b, &point_c, params.win_size, params.level, params.min_eigen);
	// compute forward-backward error
	ccv_dense_matrix_t* r0 = (ccv_dense_matrix_t*)alloca(ccv_compute_dense_matrix_size(TLD_PATCH_SIZE, TLD_PATCH_SIZE, CCV_8U | CCV_C1));
	ccv_dense_matrix_t* r1 = (ccv_dense_matrix_t*)alloca(ccv_compute_dense_matrix_size(TLD_PATCH_SIZE, TLD_PATCH_SIZE, CCV_8U | CCV_C1));
	r0 = ccv_dense_matrix_new(TLD_PATCH_SIZE, TLD_PATCH_SIZE, CCV_8U | CCV_C1, r0, 0);
	r1 = ccv_dense_matrix_new(TLD_PATCH_SIZE, TLD_PATCH_SIZE, CCV_8U | CCV_C1, r1, 0);
	int i, j, k, size;
	int* wrt = (int*)alloca(sizeof(int) * point_a->rnum);
	{ // will reclaim the stack
	float* fberr = (float*)alloca(sizeof(float) * point_a->rnum);
	float* sim = (float*)alloca(sizeof(float) * point_a->rnum);
	for (i = 0, k = 0; i < point_a->rnum; i++)
	{
		ccv_decimal_point_t* p0 = (ccv_decimal_point_t*)ccv_array_get(point_a, i);
		ccv_decimal_point_with_status_t* p1 = (ccv_decimal_point_with_status_t*)ccv_array_get(point_b, i);
		ccv_decimal_point_with_status_t* p2 = (ccv_decimal_point_with_status_t*)ccv_array_get(point_c, i);
		if (p1->status && p2->status)
		{
			fberr[k] = (p2->point.x - p0->x) * (p2->point.x - p0->x) + (p2->point.y - p0->y) * (p2->point.y - p0->y);
			ccv_decimal_slice(a, &r0, 0, p0->y - (TLD_PATCH_SIZE - 1) * 0.5, p0->x - (TLD_PATCH_SIZE - 1) * 0.5, TLD_PATCH_SIZE, TLD_PATCH_SIZE);
			ccv_decimal_slice(a, &r1, 0, p1->point.y - (TLD_PATCH_SIZE - 1) * 0.5, p1->point.x - (TLD_PATCH_SIZE - 1) * 0.5, TLD_PATCH_SIZE, TLD_PATCH_SIZE);
			sim[k] = _ccv_tld_norm_cross_correlate(r0, r1);
			wrt[k] = i;
			++k;
		}
	}
	ccv_array_free(point_c);
	if (k == 0)
	{
		// early termination because we don't have qualified tracking points
		ccv_array_free(point_b);
		ccv_array_free(point_a);
		return newbox;
	}
	size = k;
	float simmd = _ccv_tld_median(sim, 0, size - 1);
	for (i = 0, k = 0; i < size; i++)
		if (sim[i] > simmd)
		{
			fberr[k] = fberr[i];
			wrt[k] = wrt[i];
			++k;
		}
	float fberrmd = _ccv_tld_median(fberr, 0, size - 1);
	if (fberrmd >= params.min_forward_backward_error)
	{
		// early termination because we don't have qualified tracking points
		ccv_array_free(point_b);
		ccv_array_free(point_a);
		return newbox;
	}
	size = k;
	for (i = 0, k = 0; i < size; i++)
		if (fberr[i] <= fberrmd)
			wrt[k++] = wrt[i];
	if (k == 0)
	{
		// early termination because we don't have qualified tracking points
		ccv_array_free(point_b);
		ccv_array_free(point_a);
		return newbox;
	}
	} // reclaim stack
	float dx, dy;
	{ // will reclaim the stack
	float* offx = (float*)alloca(sizeof(float) * size);
	float* offy = (float*)alloca(sizeof(float) * size);
	for (i = 0; i < size; i++)
	{
		ccv_decimal_point_t* p0 = (ccv_decimal_point_t*)ccv_array_get(point_a, wrt[i]);
		ccv_decimal_point_t* p1 = (ccv_decimal_point_t*)ccv_array_get(point_b, wrt[i]);
		offx[i] = p1->x - p0->x;
		offy[i] = p1->y - p0->y;
	}
	dx = _ccv_tld_median(offx, 0, size - 1);
	dy = _ccv_tld_median(offy, 0, size - 1);
	} // reclaim stack
	if (size > 1)
	{
		float* s = (float*)alloca(sizeof(float) * size * (size - 1) / 2);
		k = 0;
		for (i = 0; i < size - 1; i++)
		{
			ccv_decimal_point_t* p0i = (ccv_decimal_point_t*)ccv_array_get(point_a, wrt[i]);
			ccv_decimal_point_t* p1i = (ccv_decimal_point_t*)ccv_array_get(point_b, wrt[i]);
			for (j = i + 1; j < size; j++)
			{
				ccv_decimal_point_t* p0j = (ccv_decimal_point_t*)ccv_array_get(point_a, wrt[j]);
				ccv_decimal_point_t* p1j = (ccv_decimal_point_t*)ccv_array_get(point_b, wrt[j]);
				s[k] = sqrtf(((p1i->x - p1j->x) * (p1i->x - p1j->x) + (p1i->y - p1j->y) * (p1i->y - p1j->y)) /
							 ((p0i->x - p0j->x) * (p0i->x - p0j->x) + (p0i->y - p0j->y) * (p0i->y - p0j->y)));
				++k;
			}
		}
		assert(size * (size - 1) / 2 == k);
		float ds = _ccv_tld_median(s, 0, size * (size - 1) / 2 - 1);
		newbox.x = (int)(box.x + dx - box.width * (ds - 1.0) * 0.5 + 0.5);
		newbox.y = (int)(box.y + dy - box.height * (ds - 1.0) * 0.5 + 0.5);
		newbox.width = (int)(box.width * ds + 0.5);
		newbox.height = (int)(box.height * ds + 0.5);
	} else {
		newbox.width = box.width;
		newbox.height = box.height;
		newbox.x = (int)(box.x + dx + 0.5);
		newbox.y = (int)(box.y + dy + 0.5);
	}
	ccv_array_free(point_b);
	ccv_array_free(point_a);
	return newbox;
}

ccv_comp_t _ccv_tld_generate_box_for(ccv_size_t image_size, ccv_rect_t box, ccv_array_t** scale, ccv_array_t** good, ccv_array_t** bad, ccv_tld_param_t params)
{
	const float SHIFT = 0.1;
	const float SCALES[] = {
		0.16151, 0.19381, 0.23257, 0.27908, 0.33490, 0.40188, 0.48225,
		0.57870, 0.69444, 0.83333, 1, 1.20000, 1.44000, 1.72800,
		2.07360, 2.48832, 2.98598, 3.58318, 4.29982, 5.15978, 6.19174
	};
	ccv_array_t* ascale = *scale = ccv_array_new(sizeof(ccv_size_t), 21, 0);
	ccv_array_t* agood = *good = ccv_array_new(sizeof(ccv_comp_t), 64, 0);
	ccv_array_t* abad = *bad = ccv_array_new(sizeof(ccv_comp_t), 64, 0);
	int s;
	double max_overlap = -DBL_MAX;
	ccv_comp_t best_box = {
		.id = 0,
		.rect = ccv_rect(0, 0, 0, 0),
	};
	for (s = 0; s < 21; s++)
	{
		int width = (int)(box.width * SCALES[s] + 0.5);
		int height = (int)(box.height * SCALES[s] + 0.5);
		int min_side = ccv_min(width, height);
		if (min_side < params.min_win || width > image_size.width || height > image_size.height)
			continue;
		ccv_size_t scale = ccv_size(width, height);
		ccv_array_push(ascale, &scale);
		for (float y = 0; y < image_size.height - height; y += SHIFT * min_side)
		{
			for (float x = 0; x < image_size.width - width; x += SHIFT * min_side)
			{
				ccv_comp_t comp;
				comp.rect = ccv_rect((int)(x + 0.5), (int)(y + 0.5), width, height);
				comp.id = ascale->rnum - 1;
				double overlap = ccv_max(ccv_min(comp.rect.x + comp.rect.width, box.x + box.width) - ccv_max(comp.rect.x, box.x), 0) *
								 ccv_max(ccv_min(comp.rect.y + comp.rect.height, box.y + box.height) - ccv_max(comp.rect.y, box.y), 0);
				overlap = overlap / (comp.rect.width * comp.rect.height + box.width * box.height - overlap);
				if (overlap > params.include_overlap)
				{
					if (overlap > max_overlap)
					{
						max_overlap = overlap;
						best_box = comp;
					}
					ccv_array_push(agood, &comp);
				} else if (overlap < params.exclude_overlap) {
					ccv_array_push(abad, &comp);
				}
			}
		}
	}
	return best_box;
}

void _ccv_tld_ferns_feature_for(ccv_ferns_t* ferns, ccv_dense_matrix_t* a, ccv_comp_t box, uint32_t* fern)
{
	ccv_dense_matrix_t roi = ccv_dense_matrix(box.rect.height, box.rect.width, CCV_GET_DATA_TYPE(a->type) | CCV_GET_CHANNEL(a->type), ccv_get_dense_matrix_cell(a, box.rect.y, box.rect.x, 0), 0);
	roi.step = a->step;
	ccv_ferns_feature(ferns, &roi, box.id, fern);
}

ccv_tld_t* ccv_tld_new(ccv_dense_matrix_t* a, ccv_rect_t box, ccv_tld_param_t params)
{
	ccv_tld_t* tld = (ccv_tld_t*)ccmalloc(sizeof(ccv_tld_t));
	tld->params = params;
	tld->box.rect = box;
	ccv_size_t image_size = ccv_size(a->cols, a->rows);
	ccv_array_t* scales = 0;
	ccv_array_t* good = 0;
	ccv_array_t* bad = 0;
	_ccv_tld_generate_box_for(image_size, box, &scales, &good, &bad, params);
	tld->ferns = ccv_ferns_new(1, 1, scales->rnum, (ccv_size_t*)ccv_array_get(scales, 0));
	tld->sv[0] = ccv_array_new(sizeof(ccv_dense_matrix_t*), 64, 0);
	tld->sv[1] = ccv_array_new(sizeof(ccv_dense_matrix_t*), 64, 0);
	sfmt_t sfmt;
	sfmt_init_gen_rand(&sfmt, (uint32_t)tld);
	sfmt_genrand_shuffle(&sfmt, ccv_array_get(bad, 0), bad->rnum, bad->rsize);
	int badex = (int)(bad->rnum * 0.5 + 0.5);
	int* idx = (int*)ccmalloc(sizeof(int) * (badex + good->rnum));
	int i, k;
	for (i = 0; i < badex + good->rnum; i++)
		idx[i] = i;
	sfmt_genrand_shuffle(&sfmt, idx, badex + good->rnum, sizeof(int));
	// train the fern classifier
	{
	uint32_t* fern = (uint32_t*)alloca(sizeof(uint32_t) * tld->ferns->structs);
	for (i = 0; i < badex + good->rnum; i++)
	{
		k = idx[i];
		if (k < badex)
		{
			ccv_comp_t* box = (ccv_comp_t*)ccv_array_get(bad, k);
			_ccv_tld_ferns_feature_for(tld->ferns, a, *box, fern);
			// fix the thresholding for negative
			if (ccv_ferns_predict(tld->ferns, fern) > tld->ferns->threshold)
				ccv_ferns_correct(tld->ferns, fern, 0, 1);
		} else {
			k -= badex;
			ccv_comp_t* box = (ccv_comp_t*)ccv_array_get(good, k);
			_ccv_tld_ferns_feature_for(tld->ferns, a, *box, fern);
			// fix the thresholding for positive
			if (ccv_ferns_predict(tld->ferns, fern) < tld->ferns->threshold)
				ccv_ferns_correct(tld->ferns, fern, 1, 1);
		}
	}
	}
	ccfree(idx);
	// train the nearest-neighbor classifier
	return tld;
}

// since there is no refcount syntax for ccv yet, we won't implicitly retain any matrix in ccv_tld_t
// instead, you should pass the previous frame and the current frame into the track function
ccv_comp_t ccv_tld_track_object(ccv_tld_t* tld, ccv_dense_matrix_t* a, ccv_dense_matrix_t* b)
{
	ccv_comp_t result;
	result.rect = _ccv_tld_short_term_track(a, b, tld->box.rect, tld->params);
	return result;
}

void ccv_tld_free(ccv_tld_t* tld)
{
	int i;
	for (i = 0; i < tld->sv[0]->rnum; i++)
		ccv_matrix_free(*(ccv_dense_matrix_t**)ccv_array_get(tld->sv[0], i));
	ccv_array_free(tld->sv[0]);
	for (i = 0; i < tld->sv[1]->rnum; i++)
		ccv_matrix_free(*(ccv_dense_matrix_t**)ccv_array_get(tld->sv[1], i));
	ccv_array_free(tld->sv[1]);
	ccv_ferns_free(tld->ferns);
	ccfree(tld);
}
