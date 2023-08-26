/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.                                               *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of HDF5.  The full HDF5 copyright notice, including     *
 * terms governing use, modification, and redistribution, is contained in    *
 * the COPYING file, which can be found at the root of the source code       *
 * distribution tree, or in https://www.hdfgroup.org/licenses.               *
 * If you do not have access to either file, you may request a copy from     *
 * help@hdfgroup.org.                                                        *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 * Purpose:     Fixed array indexed (chunked) I/O functions.
 *              The chunk coordinate is mapped as an index into an array of
 *              disk addresses for the chunks.
 *
 */

/****************/
/* Module Setup */
/****************/

#include "H5Dmodule.h" /* This source code file is part of the H5D module */

/***********/
/* Headers */
/***********/
#include "H5private.h"   /* Generic Functions            */
#include "H5Dpkg.h"      /* Datasets                     */
#include "H5Eprivate.h"  /* Error handling               */
#include "H5FAprivate.h" /* Fixed arrays                 */
#include "H5FLprivate.h" /* Free Lists                   */
#include "H5MFprivate.h" /* File space management        */
#include "H5VMprivate.h" /* Vector functions             */

/****************/
/* Local Macros */
/****************/

/* Value to fill unset array elements with */
#define H5D_FARRAY_FILL HADDR_UNDEF
#define H5D_FARRAY_FILT_FILL                                                                                 \
    {                                                                                                        \
        HADDR_UNDEF, 0, 0                                                                                    \
    }

/******************/
/* Local Typedefs */
/******************/

/* Fixed array create/open user data */
typedef struct H5D_farray_ctx_ud_t {
    const H5F_t *f;          /* Pointer to file info */
    uint32_t     chunk_size; /* Size of chunk (bytes) */
} H5D_farray_ctx_ud_t;

/* Fixed array callback context */
typedef struct H5D_farray_ctx_t {
    size_t file_addr_len;  /* Size of addresses in the file (bytes) */
    size_t chunk_size_len; /* Size of chunk sizes in the file (bytes) */
} H5D_farray_ctx_t;

/* Fixed Array callback info for iteration over chunks */
typedef struct H5D_farray_it_ud_t {
    H5D_chunk_common_ud_t common;    /* Common info for Fixed Array user data (must be first) */
    H5D_chunk_rec_t       chunk_rec; /* Generic chunk record for callback */
    hbool_t               filtered;  /* Whether the chunks are filtered */
    H5D_chunk_cb_func_t   cb;        /* Chunk callback routine */
    void                 *udata;     /* User data for chunk callback routine */
} H5D_farray_it_ud_t;

/* Native fixed array element for chunks w/filters */
typedef struct H5D_farray_filt_elmt_t {
    haddr_t  addr;        /* Address of chunk */
    uint32_t nbytes;      /* Size of chunk (in file) */
    uint32_t filter_mask; /* Excluded filters for chunk */
} H5D_farray_filt_elmt_t;

/********************/
/* Local Prototypes */
/********************/

/* Fixed Array iterator callbacks */
static int H5D__farray_idx_iterate_cb(hsize_t idx, const void *_elmt, void *_udata);
static int H5D__farray_idx_delete_cb(const H5D_chunk_rec_t *chunk_rec, void *_udata);

/* Fixed array class callbacks for chunks w/o filters */
static void  *H5D__farray_crt_context(void *udata);
static herr_t H5D__farray_dst_context(void *ctx);
static herr_t H5D__farray_fill(void *nat_blk, size_t nelmts);
static herr_t H5D__farray_encode(void *raw, const void *elmt, size_t nelmts, void *ctx);
static herr_t H5D__farray_decode(const void *raw, void *elmt, size_t nelmts, void *ctx);
static herr_t H5D__farray_debug(FILE *stream, int indent, int fwidth, hsize_t idx, const void *elmt);
static void  *H5D__farray_crt_dbg_context(H5F_t *f, haddr_t obj_addr);
static herr_t H5D__farray_dst_dbg_context(void *dbg_ctx);

/* Fixed array class callbacks for chunks w/filters */
/* (some shared with callbacks for chunks w/o filters) */
static herr_t H5D__farray_filt_fill(void *nat_blk, size_t nelmts);
static herr_t H5D__farray_filt_encode(void *raw, const void *elmt, size_t nelmts, void *ctx);
static herr_t H5D__farray_filt_decode(const void *raw, void *elmt, size_t nelmts, void *ctx);
static herr_t H5D__farray_filt_debug(FILE *stream, int indent, int fwidth, hsize_t idx, const void *elmt);

/* Chunked layout indexing callbacks */
static herr_t  H5D__farray_idx_init(const H5D_chk_idx_info_t *idx_info, const H5S_t *space,
                                    haddr_t dset_ohdr_addr);
static herr_t  H5D__farray_idx_create(const H5D_chk_idx_info_t *idx_info);
static hbool_t H5D__farray_idx_is_space_alloc(const H5O_storage_chunk_t *storage);
static herr_t  H5D__farray_idx_insert(const H5D_chk_idx_info_t *idx_info, H5D_chunk_ud_t *udata,
                                      const H5D_t *dset);
static herr_t  H5D__farray_idx_get_addr(const H5D_chk_idx_info_t *idx_info, H5D_chunk_ud_t *udata);
static int     H5D__farray_idx_iterate(const H5D_chk_idx_info_t *idx_info, H5D_chunk_cb_func_t chunk_cb,
                                       void *chunk_udata);
static herr_t  H5D__farray_idx_remove(const H5D_chk_idx_info_t *idx_info, H5D_chunk_common_ud_t *udata);
static herr_t  H5D__farray_idx_delete(const H5D_chk_idx_info_t *idx_info);
static herr_t  H5D__farray_idx_copy_setup(const H5D_chk_idx_info_t *idx_info_src,
                                          const H5D_chk_idx_info_t *idx_info_dst);
static herr_t  H5D__farray_idx_copy_shutdown(H5O_storage_chunk_t *storage_src,
                                             H5O_storage_chunk_t *storage_dst);
static herr_t  H5D__farray_idx_size(const H5D_chk_idx_info_t *idx_info, hsize_t *size);
static herr_t  H5D__farray_idx_reset(H5O_storage_chunk_t *storage, hbool_t reset_addr);
static herr_t  H5D__farray_idx_dump(const H5O_storage_chunk_t *storage, FILE *stream);
static herr_t  H5D__farray_idx_dest(const H5D_chk_idx_info_t *idx_info);

/* Generic fixed array routines */
static herr_t H5D__farray_idx_open(const H5D_chk_idx_info_t *idx_info);
static herr_t H5D__farray_idx_depend(const H5D_chk_idx_info_t *idx_info);

/*********************/
/* Package Variables */
/*********************/

/* Fixed array indexed chunk I/O ops */
const H5D_chunk_ops_t H5D_COPS_FARRAY[1] = {{
    TRUE,                           /* Fixed array indices support SWMR access */
    H5D__farray_idx_init,           /* init */
    H5D__farray_idx_create,         /* create */
    H5D__farray_idx_is_space_alloc, /* is_space_alloc */
    H5D__farray_idx_insert,         /* insert */
    H5D__farray_idx_get_addr,       /* get_addr */
    NULL,                           /* resize */
    H5D__farray_idx_iterate,        /* iterate */
    H5D__farray_idx_remove,         /* remove */
    H5D__farray_idx_delete,         /* delete */
    H5D__farray_idx_copy_setup,     /* copy_setup */
    H5D__farray_idx_copy_shutdown,  /* copy_shutdown */
    H5D__farray_idx_size,           /* size */
    H5D__farray_idx_reset,          /* reset */
    H5D__farray_idx_dump,           /* dump */
    H5D__farray_idx_dest            /* destroy */
}};

/*****************************/
/* Library Private Variables */
/*****************************/

/*******************/
/* Local Variables */
/*******************/

/* Fixed array class callbacks for dataset chunks w/o filters */
const H5FA_class_t H5FA_CLS_CHUNK[1] = {{
    H5FA_CLS_CHUNK_ID,           /* Type of fixed array */
    "Chunk w/o filters",         /* Name of fixed array class */
    sizeof(haddr_t),             /* Size of native element */
    H5D__farray_crt_context,     /* Create context */
    H5D__farray_dst_context,     /* Destroy context */
    H5D__farray_fill,            /* Fill block of missing elements callback */
    H5D__farray_encode,          /* Element encoding callback */
    H5D__farray_decode,          /* Element decoding callback */
    H5D__farray_debug,           /* Element debugging callback */
    H5D__farray_crt_dbg_context, /* Create debugging context */
    H5D__farray_dst_dbg_context  /* Destroy debugging context */
}};

/* Fixed array class callbacks for dataset chunks w/filters */
const H5FA_class_t H5FA_CLS_FILT_CHUNK[1] = {{
    H5FA_CLS_FILT_CHUNK_ID,         /* Type of fixed array */
    "Chunk w/filters",              /* Name of fixed array class */
    sizeof(H5D_farray_filt_elmt_t), /* Size of native element */
    H5D__farray_crt_context,        /* Create context */
    H5D__farray_dst_context,        /* Destroy context */
    H5D__farray_filt_fill,          /* Fill block of missing elements callback */
    H5D__farray_filt_encode,        /* Element encoding callback */
    H5D__farray_filt_decode,        /* Element decoding callback */
    H5D__farray_filt_debug,         /* Element debugging callback */
    H5D__farray_crt_dbg_context,    /* Create debugging context */
    H5D__farray_dst_dbg_context     /* Destroy debugging context */
}};

/* Declare a free list to manage the H5D_farray_ctx_t struct */
H5FL_DEFINE_STATIC(H5D_farray_ctx_t);

/* Declare a free list to manage the H5D_farray_ctx_ud_t struct */
H5FL_DEFINE_STATIC(H5D_farray_ctx_ud_t);

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_crt_context
 *
 * Purpose:     Create context for callbacks
 *
 * Return:      Success:    non-NULL
 *              Failure:    NULL
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static void *
H5D__farray_crt_context(void *_udata)
{
    H5D_farray_ctx_t    *ctx;                                       /* Fixed array callback context */
    H5D_farray_ctx_ud_t *udata     = (H5D_farray_ctx_ud_t *)_udata; /* User data for fixed array context */
    void                *ret_value = NULL;                          /* Return value */

    FUNC_ENTER_STATIC

    /* Sanity checks */
    HDassert(udata);
    HDassert(udata->f);
    HDassert(udata->chunk_size > 0);

    /* Allocate new context structure */
    if (NULL == (ctx = H5FL_MALLOC(H5D_farray_ctx_t)))
        HGOTO_ERROR(H5E_DATASET, H5E_CANTALLOC, NULL, "can't allocate fixed array client callback context")

    /* Initialize the context */
    ctx->file_addr_len = H5F_SIZEOF_ADDR(udata->f);

    /* Compute the size required for encoding the size of a chunk, allowing
     *      for an extra byte, in case the filter makes the chunk larger.
     */
    ctx->chunk_size_len = 1 + ((H5VM_log2_gen((uint64_t)udata->chunk_size) + 8) / 8);
    if (ctx->chunk_size_len > 8)
        ctx->chunk_size_len = 8;

    /* Set return value */
    ret_value = ctx;

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5D__farray_crt_context() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_dst_context
 *
 * Purpose:     Destroy context for callbacks
 *
 * Return:      Success:    non-NULL
 *              Failure:    NULL
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_dst_context(void *_ctx)
{
    H5D_farray_ctx_t *ctx = (H5D_farray_ctx_t *)_ctx; /* Fixed array callback context */

    FUNC_ENTER_STATIC_NOERR

    /* Sanity checks */
    HDassert(ctx);

    /* Release context structure */
    ctx = H5FL_FREE(H5D_farray_ctx_t, ctx);

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5D__farray_dst_context() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_fill
 *
 * Purpose:     Fill "missing elements" in block of elements
 *
 * Return:      Success:    non-negative
 *              Failure:    negative
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_fill(void *nat_blk, size_t nelmts)
{
    haddr_t fill_val = H5D_FARRAY_FILL; /* Value to fill elements with */

    FUNC_ENTER_STATIC_NOERR

    /* Sanity checks */
    HDassert(nat_blk);
    HDassert(nelmts);

    H5VM_array_fill(nat_blk, &fill_val, H5FA_CLS_CHUNK->nat_elmt_size, nelmts);

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5D__farray_fill() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_encode
 *
 * Purpose:     Encode an element from "native" to "raw" form
 *
 * Return:      Success:    non-negative
 *              Failure:    negative
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_encode(void *raw, const void *_elmt, size_t nelmts, void *_ctx)
{
    H5D_farray_ctx_t *ctx  = (H5D_farray_ctx_t *)_ctx; /* Fixed array callback context */
    const haddr_t    *elmt = (const haddr_t *)_elmt;   /* Convenience pointer to native elements */

    FUNC_ENTER_STATIC_NOERR

    /* Sanity checks */
    HDassert(raw);
    HDassert(elmt);
    HDassert(nelmts);
    HDassert(ctx);

    /* Encode native elements into raw elements */
    while (nelmts) {
        /* Encode element */
        /* (advances 'raw' pointer) */
        H5F_addr_encode_len(ctx->file_addr_len, (uint8_t **)&raw, *elmt);

        /* Advance native element pointer */
        elmt++;

        /* Decrement # of elements to encode */
        nelmts--;
    } /* end while */

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5D__farray_encode() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_decode
 *
 * Purpose:     Decode an element from "raw" to "native" form
 *
 * Return:      Success:    non-negative
 *              Failure:    negative
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_decode(const void *_raw, void *_elmt, size_t nelmts, void *_ctx)
{
    H5D_farray_ctx_t *ctx  = (H5D_farray_ctx_t *)_ctx; /* Fixed array callback context */
    haddr_t          *elmt = (haddr_t *)_elmt;         /* Convenience pointer to native elements */
    const uint8_t    *raw  = (const uint8_t *)_raw;    /* Convenience pointer to raw elements */

    FUNC_ENTER_STATIC_NOERR

    /* Sanity checks */
    HDassert(raw);
    HDassert(elmt);
    HDassert(nelmts);

    /* Decode raw elements into native elements */
    while (nelmts) {
        /* Decode element */
        /* (advances 'raw' pointer) */
        H5F_addr_decode_len(ctx->file_addr_len, &raw, elmt);

        /* Advance native element pointer */
        elmt++;

        /* Decrement # of elements to decode */
        nelmts--;
    } /* end while */

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5D__farray_decode() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_debug
 *
 * Purpose:     Display an element for debugging
 *
 * Return:      Success:    non-negative
 *              Failure:    negative
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_debug(FILE *stream, int indent, int fwidth, hsize_t idx, const void *elmt)
{
    char temp_str[128]; /* Temporary string, for formatting */

    FUNC_ENTER_STATIC_NOERR

    /* Sanity checks */
    HDassert(stream);
    HDassert(elmt);

    /* Print element */
    HDsprintf(temp_str, "Element #%" PRIuHSIZE ":", idx);
    HDfprintf(stream, "%*s%-*s %" PRIuHADDR "\n", indent, "", fwidth, temp_str, *(const haddr_t *)elmt);

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5D__farray_debug() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_crt_dbg_context
 *
 * Purpose:     Create context for debugging callback
 *              (get the layout message in the specified object header)
 *
 * Return:      Success:    non-NULL
 *              Failure:    NULL
 *
 * Programmer:  Vailin Choi
 *              5th August, 2009
 *
 *-------------------------------------------------------------------------
 */
static void *
H5D__farray_crt_dbg_context(H5F_t *f, haddr_t obj_addr)
{
    H5D_farray_ctx_ud_t *dbg_ctx = NULL;     /* Context for fixed array callback */
    H5O_loc_t            obj_loc;            /* Pointer to an object's location */
    hbool_t              obj_opened = FALSE; /* Flag to indicate that the object header was opened */
    H5O_layout_t         layout;             /* Layout message */
    void                *ret_value = NULL;   /* Return value */

    FUNC_ENTER_STATIC

    /* Sanity checks */
    HDassert(f);
    HDassert(H5F_addr_defined(obj_addr));

    /* Allocate context for debugging callback */
    if (NULL == (dbg_ctx = H5FL_MALLOC(H5D_farray_ctx_ud_t)))
        HGOTO_ERROR(H5E_DATASET, H5E_CANTALLOC, NULL, "can't allocate fixed array client callback context")

    /* Set up the object header location info */
    H5O_loc_reset(&obj_loc);
    obj_loc.file = f;
    obj_loc.addr = obj_addr;

    /* Open the object header where the layout message resides */
    if (H5O_open(&obj_loc) < 0)
        HGOTO_ERROR(H5E_DATASET, H5E_CANTOPENOBJ, NULL, "can't open object header")
    obj_opened = TRUE;

    /* Read the layout message */
    if (NULL == H5O_msg_read(&obj_loc, H5O_LAYOUT_ID, &layout))
        HGOTO_ERROR(H5E_DATASET, H5E_CANTGET, NULL, "can't get layout info")

    /* close the object header */
    if (H5O_close(&obj_loc, NULL) < 0)
        HGOTO_ERROR(H5E_DATASET, H5E_CANTCLOSEOBJ, NULL, "can't close object header")

    /* Create user data */
    dbg_ctx->f          = f;
    dbg_ctx->chunk_size = layout.u.chunk.size;

    /* Set return value */
    ret_value = dbg_ctx;

done:
    /* Cleanup on error */
    if (ret_value == NULL) {
        /* Release context structure */
        if (dbg_ctx)
            dbg_ctx = H5FL_FREE(H5D_farray_ctx_ud_t, dbg_ctx);

        /* Close object header */
        if (obj_opened)
            if (H5O_close(&obj_loc, NULL) < 0)
                HDONE_ERROR(H5E_DATASET, H5E_CANTCLOSEOBJ, NULL, "can't close object header")
    } /* end if */

    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5D__farray_crt_dbg_context() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_dst_dbg_context
 *
 * Purpose:     Destroy context for debugging callback
 *              (free the layout message from the specified object header)
 *
 * Return:      Success:    non-negative
 *              Failure:    negative
 *
 * Programmer:  Quincey Koziol
 *              24th September, 2009
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_dst_dbg_context(void *_dbg_ctx)
{
    H5D_farray_ctx_ud_t *dbg_ctx = (H5D_farray_ctx_ud_t *)_dbg_ctx; /* Context for fixed array callback */

    FUNC_ENTER_STATIC_NOERR

    /* Sanity checks */
    HDassert(dbg_ctx);

    /* Release context structure */
    dbg_ctx = H5FL_FREE(H5D_farray_ctx_ud_t, dbg_ctx);

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5D__farray_dst_dbg_context() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_filt_fill
 *
 * Purpose:     Fill "missing elements" in block of elements
 *
 * Return:      Success:    non-negative
 *              Failure:    negative
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_filt_fill(void *nat_blk, size_t nelmts)
{
    H5D_farray_filt_elmt_t fill_val = H5D_FARRAY_FILT_FILL; /* Value to fill elements with */

    FUNC_ENTER_STATIC_NOERR

    /* Sanity checks */
    HDassert(nat_blk);
    HDassert(nelmts);
    HDassert(sizeof(fill_val) == H5FA_CLS_FILT_CHUNK->nat_elmt_size);

    H5VM_array_fill(nat_blk, &fill_val, H5FA_CLS_FILT_CHUNK->nat_elmt_size, nelmts);

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5D__farray_filt_fill() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_filt_encode
 *
 * Purpose:     Encode an element from "native" to "raw" form
 *
 * Return:      Success:    non-negative
 *              Failure:    negative
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_filt_encode(void *_raw, const void *_elmt, size_t nelmts, void *_ctx)
{
    H5D_farray_ctx_t             *ctx = (H5D_farray_ctx_t *)_ctx; /* Fixed array callback context */
    uint8_t                      *raw = (uint8_t *)_raw;          /* Convenience pointer to raw elements */
    const H5D_farray_filt_elmt_t *elmt =
        (const H5D_farray_filt_elmt_t *)_elmt; /* Convenience pointer to native elements */

    FUNC_ENTER_STATIC_NOERR

    /* Sanity checks */
    HDassert(raw);
    HDassert(elmt);
    HDassert(nelmts);
    HDassert(ctx);

    /* Encode native elements into raw elements */
    while (nelmts) {
        /* Encode element */
        /* (advances 'raw' pointer) */
        H5F_addr_encode_len(ctx->file_addr_len, &raw, elmt->addr);
        UINT64ENCODE_VAR(raw, elmt->nbytes, ctx->chunk_size_len);
        UINT32ENCODE(raw, elmt->filter_mask);

        /* Advance native element pointer */
        elmt++;

        /* Decrement # of elements to encode */
        nelmts--;
    } /* end while */

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5D__farray_filt_encode() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_filt_decode
 *
 * Purpose:     Decode an element from "raw" to "native" form
 *
 * Return:      Success:    non-negative
 *              Failure:    negative
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_filt_decode(const void *_raw, void *_elmt, size_t nelmts, void *_ctx)
{
    H5D_farray_ctx_t       *ctx = (H5D_farray_ctx_t *)_ctx; /* Fixed array callback context */
    H5D_farray_filt_elmt_t *elmt =
        (H5D_farray_filt_elmt_t *)_elmt;        /* Convenience pointer to native elements */
    const uint8_t *raw = (const uint8_t *)_raw; /* Convenience pointer to raw elements */

    FUNC_ENTER_STATIC_NOERR

    /* Sanity checks */
    HDassert(raw);
    HDassert(elmt);
    HDassert(nelmts);

    /* Decode raw elements into native elements */
    while (nelmts) {
        /* Decode element */
        /* (advances 'raw' pointer) */
        H5F_addr_decode_len(ctx->file_addr_len, &raw, &elmt->addr);
        UINT64DECODE_VAR(raw, elmt->nbytes, ctx->chunk_size_len);
        UINT32DECODE(raw, elmt->filter_mask);

        /* Advance native element pointer */
        elmt++;

        /* Decrement # of elements to decode */
        nelmts--;
    } /* end while */

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5D__farray_filt_decode() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_filt_debug
 *
 * Purpose:     Display an element for debugging
 *
 * Return:      Success:    non-negative
 *              Failure:    negative
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_filt_debug(FILE *stream, int indent, int fwidth, hsize_t idx, const void *_elmt)
{
    const H5D_farray_filt_elmt_t *elmt =
        (const H5D_farray_filt_elmt_t *)_elmt; /* Convenience pointer to native elements */
    char temp_str[128];                        /* Temporary string, for formatting */

    FUNC_ENTER_STATIC_NOERR

    /* Sanity checks */
    HDassert(stream);
    HDassert(elmt);

    /* Print element */
    HDsprintf(temp_str, "Element #%" PRIuHSIZE ":", idx);
    HDfprintf(stream, "%*s%-*s {%" PRIuHADDR ", %u, %0x}\n", indent, "", fwidth, temp_str, elmt->addr,
              elmt->nbytes, elmt->filter_mask);

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5D__farray_filt_debug() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_idx_depend
 *
 * Purpose:     Create flush dependency between fixed array and dataset's
 *              object header.
 *
 * Return:      Success:    non-negative
 *              Failure:    negative
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_idx_depend(const H5D_chk_idx_info_t *idx_info)
{
    H5O_t              *oh = NULL;           /* Object header */
    H5O_loc_t           oloc;                /* Temporary object header location for dataset */
    H5AC_proxy_entry_t *oh_proxy;            /* Dataset's object header proxy */
    herr_t              ret_value = SUCCEED; /* Return value */

    FUNC_ENTER_STATIC

    /* Check args */
    HDassert(idx_info);
    HDassert(idx_info->f);
    HDassert(H5F_INTENT(idx_info->f) & H5F_ACC_SWMR_WRITE);
    HDassert(idx_info->pline);
    HDassert(idx_info->layout);
    HDassert(H5D_CHUNK_IDX_FARRAY == idx_info->layout->idx_type);
    HDassert(idx_info->storage);
    HDassert(H5D_CHUNK_IDX_FARRAY == idx_info->storage->idx_type);
    HDassert(H5F_addr_defined(idx_info->storage->idx_addr));
    HDassert(idx_info->storage->u.farray.fa);

    /* Set up object header location for dataset */
    H5O_loc_reset(&oloc);
    oloc.file = idx_info->f;
    oloc.addr = idx_info->storage->u.farray.dset_ohdr_addr;

    /* Get header */
    if (NULL == (oh = H5O_protect(&oloc, H5AC__READ_ONLY_FLAG, TRUE)))
        HGOTO_ERROR(H5E_DATASET, H5E_CANTPROTECT, FAIL, "unable to protect object header")

    /* Retrieve the dataset's object header proxy */
    if (NULL == (oh_proxy = H5O_get_proxy(oh)))
        HGOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "unable to get dataset object header proxy")

    /* Make the fixed array a child flush dependency of the dataset's object header proxy */
    if (H5FA_depend(idx_info->storage->u.farray.fa, oh_proxy) < 0)
        HGOTO_ERROR(H5E_DATASET, H5E_CANTDEPEND, FAIL,
                    "unable to create flush dependency on object header proxy")

done:
    /* Release the object header from the cache */
    if (oh && H5O_unprotect(&oloc, oh, H5AC__NO_FLAGS_SET) < 0)
        HDONE_ERROR(H5E_DATASET, H5E_CANTUNPROTECT, FAIL, "unable to release object header")

    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5D__farray_idx_depend() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_idx_init
 *
 * Purpose:     Initialize the indexing information for a dataset.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Neil Fortner
 *              Wednensday, May 23, 2012
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_idx_init(const H5D_chk_idx_info_t *idx_info, const H5S_t H5_ATTR_UNUSED *space,
                     haddr_t dset_ohdr_addr)
{
    FUNC_ENTER_STATIC_NOERR

    /* Check args */
    HDassert(idx_info);
    HDassert(idx_info->storage);
    HDassert(H5F_addr_defined(dset_ohdr_addr));

    idx_info->storage->u.farray.dset_ohdr_addr = dset_ohdr_addr;

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5D__farray_idx_init() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_idx_open
 *
 * Purpose:     Opens an existing fixed array and initializes
 *              the layout struct with information about the storage.
 *
 * Return:      Success:    non-negative
 *              Failure:    negative
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_idx_open(const H5D_chk_idx_info_t *idx_info)
{
    H5D_farray_ctx_ud_t udata;               /* User data for fixed array open call */
    herr_t              ret_value = SUCCEED; /* Return value */

    FUNC_ENTER_STATIC

    /* Check args */
    HDassert(idx_info);
    HDassert(idx_info->f);
    HDassert(idx_info->pline);
    HDassert(idx_info->layout);
    HDassert(H5D_CHUNK_IDX_FARRAY == idx_info->layout->idx_type);
    HDassert(idx_info->storage);
    HDassert(H5D_CHUNK_IDX_FARRAY == idx_info->storage->idx_type);
    HDassert(H5F_addr_defined(idx_info->storage->idx_addr));
    HDassert(NULL == idx_info->storage->u.farray.fa);

    /* Set up the user data */
    udata.f          = idx_info->f;
    udata.chunk_size = idx_info->layout->size;

    /* Open the fixed array for the chunk index */
    if (NULL ==
        (idx_info->storage->u.farray.fa = H5FA_open(idx_info->f, idx_info->storage->idx_addr, &udata)))
        HGOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't open fixed array")

    /* Check for SWMR writes to the file */
    if (H5F_INTENT(idx_info->f) & H5F_ACC_SWMR_WRITE)
        if (H5D__farray_idx_depend(idx_info) < 0)
            HGOTO_ERROR(H5E_DATASET, H5E_CANTDEPEND, FAIL,
                        "unable to create flush dependency on object header")

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5D__farray_idx_open() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_idx_create
 *
 * Purpose:     Creates a new indexed-storage fixed array and initializes
 *              the layout struct with information about the storage.  The
 *              struct should be immediately written to the object header.
 *
 *              This function must be called before passing LAYOUT to any of
 *              the other indexed storage functions!
 *
 * Return:      Non-negative on success (with the LAYOUT argument initialized
 *              and ready to write to an object header). Negative on failure.
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_idx_create(const H5D_chk_idx_info_t *idx_info)
{
    H5FA_create_t       cparam;              /* Fixed array creation parameters */
    H5D_farray_ctx_ud_t udata;               /* User data for fixed array create call */
    herr_t              ret_value = SUCCEED; /* Return value */

    FUNC_ENTER_STATIC

    /* Check args */
    HDassert(idx_info);
    HDassert(idx_info->f);
    HDassert(idx_info->pline);
    HDassert(idx_info->layout);
    HDassert(idx_info->storage);
    HDassert(!H5F_addr_defined(idx_info->storage->idx_addr));
    HDassert(NULL == idx_info->storage->u.farray.fa);
    HDassert(idx_info->layout->nchunks);

    /* General parameters */
    if (idx_info->pline->nused > 0) {
        unsigned chunk_size_len; /* Size of encoded chunk size */

        /* Compute the size required for encoding the size of a chunk, allowing
         *      for an extra byte, in case the filter makes the chunk larger.
         */
        chunk_size_len = 1 + ((H5VM_log2_gen((uint64_t)idx_info->layout->size) + 8) / 8);
        if (chunk_size_len > 8)
            chunk_size_len = 8;

        cparam.cls           = H5FA_CLS_FILT_CHUNK;
        cparam.raw_elmt_size = (uint8_t)(H5F_SIZEOF_ADDR(idx_info->f) + chunk_size_len + 4);
    } /* end if */
    else {
        cparam.cls           = H5FA_CLS_CHUNK;
        cparam.raw_elmt_size = (uint8_t)H5F_SIZEOF_ADDR(idx_info->f);
    } /* end else */
    cparam.max_dblk_page_nelmts_bits = idx_info->layout->u.farray.cparam.max_dblk_page_nelmts_bits;
    HDassert(cparam.max_dblk_page_nelmts_bits > 0);
    cparam.nelmts = idx_info->layout->max_nchunks;

    /* Set up the user data */
    udata.f          = idx_info->f;
    udata.chunk_size = idx_info->layout->size;

    /* Create the fixed array for the chunk index */
    if (NULL == (idx_info->storage->u.farray.fa = H5FA_create(idx_info->f, &cparam, &udata)))
        HGOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "can't create fixed array")

    /* Get the address of the fixed array in file */
    if (H5FA_get_addr(idx_info->storage->u.farray.fa, &(idx_info->storage->idx_addr)) < 0)
        HGOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't query fixed array address")

    /* Check for SWMR writes to the file */
    if (H5F_INTENT(idx_info->f) & H5F_ACC_SWMR_WRITE)
        if (H5D__farray_idx_depend(idx_info) < 0)
            HGOTO_ERROR(H5E_DATASET, H5E_CANTDEPEND, FAIL,
                        "unable to create flush dependency on object header")

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5D__farray_idx_create() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_idx_is_space_alloc
 *
 * Purpose:     Query if space is allocated for index method
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static hbool_t
H5D__farray_idx_is_space_alloc(const H5O_storage_chunk_t *storage)
{
    FUNC_ENTER_STATIC_NOERR

    /* Check args */
    HDassert(storage);

    FUNC_LEAVE_NOAPI((hbool_t)H5F_addr_defined(storage->idx_addr))
} /* end H5D__farray_idx_is_space_alloc() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_idx_insert
 *
 * Purpose:     Insert chunk address into the indexing structure.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Vailin Choi; 5 May 2014
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_idx_insert(const H5D_chk_idx_info_t *idx_info, H5D_chunk_ud_t *udata,
                       const H5D_t H5_ATTR_UNUSED *dset)
{
    H5FA_t *fa;                  /* Pointer to fixed array structure */
    herr_t  ret_value = SUCCEED; /* Return value */

    FUNC_ENTER_STATIC

    /* Sanity checks */
    HDassert(idx_info);
    HDassert(idx_info->f);
    HDassert(idx_info->pline);
    HDassert(idx_info->layout);
    HDassert(idx_info->storage);
    HDassert(H5F_addr_defined(idx_info->storage->idx_addr));
    HDassert(udata);

    /* Check if the fixed array is open yet */
    if (NULL == idx_info->storage->u.farray.fa) {
        /* Open the fixed array in file */
        if (H5D__farray_idx_open(idx_info) < 0)
            HGOTO_ERROR(H5E_DATASET, H5E_CANTOPENOBJ, FAIL, "can't open fixed array")
    }
    else /* Patch the top level file pointer contained in fa if needed */
        H5FA_patch_file(idx_info->storage->u.farray.fa, idx_info->f);

    /* Set convenience pointer to fixed array structure */
    fa = idx_info->storage->u.farray.fa;

    if (!H5F_addr_defined(udata->chunk_block.offset))
        HGOTO_ERROR(H5E_DATASET, H5E_CANTALLOC, FAIL, "The chunk should have allocated already")
    if (udata->chunk_idx != (udata->chunk_idx & 0xffffffff)) /* negative value */
        HGOTO_ERROR(H5E_ARGS, H5E_BADRANGE, FAIL, "chunk index must be less than 2^32")

    /* Check for filters on chunks */
    if (idx_info->pline->nused > 0) {
        H5D_farray_filt_elmt_t elmt; /* Fixed array element */

        elmt.addr = udata->chunk_block.offset;
        H5_CHECKED_ASSIGN(elmt.nbytes, uint32_t, udata->chunk_block.length, hsize_t);
        elmt.filter_mask = udata->filter_mask;

        /* Set the info for the chunk */
        if (H5FA_set(fa, udata->chunk_idx, &elmt) < 0)
            HGOTO_ERROR(H5E_DATASET, H5E_CANTSET, FAIL, "can't set chunk info")
    } /* end if */
    else {
        /* Set the address for the chunk */
        if (H5FA_set(fa, udata->chunk_idx, &udata->chunk_block.offset) < 0)
            HGOTO_ERROR(H5E_DATASET, H5E_CANTSET, FAIL, "can't set chunk address")
    } /* end else */

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5D__farray_idx_insert() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_idx_get_addr
 *
 * Purpose:     Get the file address of a chunk if file space has been
 *              assigned.  Save the retrieved information in the udata
 *              supplied.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_idx_get_addr(const H5D_chk_idx_info_t *idx_info, H5D_chunk_ud_t *udata)
{
    H5FA_t *fa;                  /* Pointer to fixed array structure */
    hsize_t idx;                 /* Array index of chunk */
    herr_t  ret_value = SUCCEED; /* Return value */

    FUNC_ENTER_STATIC

    /* Sanity checks */
    HDassert(idx_info);
    HDassert(idx_info->f);
    HDassert(idx_info->pline);
    HDassert(idx_info->layout);
    HDassert(idx_info->storage);
    HDassert(H5F_addr_defined(idx_info->storage->idx_addr));
    HDassert(udata);

    /* Check if the fixed array is open yet */
    if (NULL == idx_info->storage->u.farray.fa) {
        /* Open the fixed array in file */
        if (H5D__farray_idx_open(idx_info) < 0)
            HGOTO_ERROR(H5E_DATASET, H5E_CANTOPENOBJ, FAIL, "can't open fixed array")
    }
    else /* Patch the top level file pointer contained in fa if needed */
        H5FA_patch_file(idx_info->storage->u.farray.fa, idx_info->f);

    /* Set convenience pointer to fixed array structure */
    fa = idx_info->storage->u.farray.fa;

    /* Calculate the index of this chunk */
    idx = H5VM_array_offset_pre((idx_info->layout->ndims - 1), idx_info->layout->max_down_chunks,
                                udata->common.scaled);

    udata->chunk_idx = idx;

    /* Check for filters on chunks */
    if (idx_info->pline->nused > 0) {
        H5D_farray_filt_elmt_t elmt; /* Fixed array element */

        /* Get the information for the chunk */
        if (H5FA_get(fa, idx, &elmt) < 0)
            HGOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get chunk info")

        /* Set the info for the chunk */
        udata->chunk_block.offset = elmt.addr;
        udata->chunk_block.length = elmt.nbytes;
        udata->filter_mask        = elmt.filter_mask;
    } /* end if */
    else {
        /* Get the address for the chunk */
        if (H5FA_get(fa, idx, &udata->chunk_block.offset) < 0)
            HGOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get chunk address")

        /* Update the other (constant) information for the chunk */
        udata->chunk_block.length = idx_info->layout->size;
        udata->filter_mask        = 0;
    } /* end else */

    if (!H5F_addr_defined(udata->chunk_block.offset))
        udata->chunk_block.length = 0;

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5D__farray_idx_get_addr() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_idx_iterate_cb
 *
 * Purpose:     Callback routine for fixed array element iteration.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static int
H5D__farray_idx_iterate_cb(hsize_t H5_ATTR_UNUSED idx, const void *_elmt, void *_udata)
{
    H5D_farray_it_ud_t *udata = (H5D_farray_it_ud_t *)_udata; /* User data */
    unsigned            ndims;                                /* Rank of chunk */
    int                 curr_dim;                             /* Current dimension */
    int                 ret_value = H5_ITER_CONT;             /* Return value */

    FUNC_ENTER_STATIC_NOERR

    /* Compose generic chunk record for callback */
    if (udata->filtered) {
        const H5D_farray_filt_elmt_t *filt_elmt = (const H5D_farray_filt_elmt_t *)_elmt;

        udata->chunk_rec.chunk_addr  = filt_elmt->addr;
        udata->chunk_rec.nbytes      = filt_elmt->nbytes;
        udata->chunk_rec.filter_mask = filt_elmt->filter_mask;
    } /* end if */
    else
        udata->chunk_rec.chunk_addr = *(const haddr_t *)_elmt;

    /* Make "generic chunk" callback */
    if (H5F_addr_defined(udata->chunk_rec.chunk_addr))
        if ((ret_value = (udata->cb)(&udata->chunk_rec, udata->udata)) < 0)
            HERROR(H5E_DATASET, H5E_CALLBACK, "failure in generic chunk iterator callback");

    /* Update coordinates of chunk in dataset */
    ndims = udata->common.layout->ndims - 1;
    HDassert(ndims > 0);
    curr_dim = (int)(ndims - 1);
    while (curr_dim >= 0) {
        /* Increment coordinate in current dimension */
        udata->chunk_rec.scaled[curr_dim]++;

        /* Check if we went off the end of the current dimension */
        if (udata->chunk_rec.scaled[curr_dim] >= udata->common.layout->max_chunks[curr_dim]) {
            /* Reset coordinate & move to next faster dimension */
            udata->chunk_rec.scaled[curr_dim] = 0;
            curr_dim--;
        } /* end if */
        else
            break;
    } /* end while */

    FUNC_LEAVE_NOAPI(ret_value)
} /* H5D__farray_idx_iterate_cb() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_idx_iterate
 *
 * Purpose:     Iterate over the chunks in an index, making a callback
 *              for each one.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static int
H5D__farray_idx_iterate(const H5D_chk_idx_info_t *idx_info, H5D_chunk_cb_func_t chunk_cb, void *chunk_udata)
{
    H5FA_t     *fa;               /* Pointer to fixed array structure */
    H5FA_stat_t fa_stat;          /* Fixed array statistics */
    int         ret_value = FAIL; /* Return value */

    FUNC_ENTER_STATIC

    /* Sanity checks */
    HDassert(idx_info);
    HDassert(idx_info->f);
    HDassert(idx_info->pline);
    HDassert(idx_info->layout);
    HDassert(idx_info->storage);
    HDassert(H5F_addr_defined(idx_info->storage->idx_addr));
    HDassert(chunk_cb);
    HDassert(chunk_udata);

    /* Check if the fixed array is open yet */
    if (NULL == idx_info->storage->u.farray.fa) {
        /* Open the fixed array in file */
        if (H5D__farray_idx_open(idx_info) < 0)
            HGOTO_ERROR(H5E_DATASET, H5E_CANTOPENOBJ, FAIL, "can't open fixed array")
    }
    else /* Patch the top level file pointer contained in fa if needed */
        H5FA_patch_file(idx_info->storage->u.farray.fa, idx_info->f);

    /* Set convenience pointer to fixed array structure */
    fa = idx_info->storage->u.farray.fa;

    /* Get the fixed array statistics */
    if (H5FA_get_stats(fa, &fa_stat) < 0)
        HGOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't query fixed array statistics")

    /* Check if there are any array elements */
    if (fa_stat.nelmts > 0) {
        H5D_farray_it_ud_t udata; /* User data for iteration callback */

        /* Initialize userdata */
        HDmemset(&udata, 0, sizeof udata);
        udata.common.layout  = idx_info->layout;
        udata.common.storage = idx_info->storage;
        HDmemset(&udata.chunk_rec, 0, sizeof(udata.chunk_rec));
        udata.filtered = (idx_info->pline->nused > 0);
        if (!udata.filtered) {
            udata.chunk_rec.nbytes      = idx_info->layout->size;
            udata.chunk_rec.filter_mask = 0;
        } /* end if */
        udata.cb    = chunk_cb;
        udata.udata = chunk_udata;

        /* Iterate over the fixed array elements */
        if ((ret_value = H5FA_iterate(fa, H5D__farray_idx_iterate_cb, &udata)) < 0)
            HERROR(H5E_DATASET, H5E_BADITER, "unable to iterate over fixed array chunk index");
    } /* end if */

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5D__farray_idx_iterate() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_idx_remove
 *
 * Purpose:     Remove chunk from index.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_idx_remove(const H5D_chk_idx_info_t *idx_info, H5D_chunk_common_ud_t *udata)
{
    H5FA_t *fa;                  /* Pointer to fixed array structure */
    hsize_t idx;                 /* Array index of chunk */
    herr_t  ret_value = SUCCEED; /* Return value */

    FUNC_ENTER_STATIC

    /* Sanity checks */
    HDassert(idx_info);
    HDassert(idx_info->f);
    HDassert(idx_info->pline);
    HDassert(idx_info->layout);
    HDassert(idx_info->storage);
    HDassert(H5F_addr_defined(idx_info->storage->idx_addr));
    HDassert(udata);

    /* Check if the fixed array is open yet */
    if (NULL == idx_info->storage->u.farray.fa) {
        /* Open the fixed array in file */
        if (H5D__farray_idx_open(idx_info) < 0)
            HGOTO_ERROR(H5E_DATASET, H5E_CANTOPENOBJ, FAIL, "can't open fixed array")
    }
    else /* Patch the top level file pointer contained in fa if needed */
        if (H5FA_patch_file(idx_info->storage->u.farray.fa, idx_info->f) < 0)
            HGOTO_ERROR(H5E_DATASET, H5E_CANTOPENOBJ, FAIL, "can't patch fixed array file pointer")

    /* Set convenience pointer to fixed array structure */
    fa = idx_info->storage->u.farray.fa;

    /* Calculate the index of this chunk */
    idx = H5VM_array_offset_pre((idx_info->layout->ndims - 1), idx_info->layout->max_down_chunks,
                                udata->scaled);

    /* Check for filters on chunks */
    if (idx_info->pline->nused > 0) {
        H5D_farray_filt_elmt_t elmt; /* Fixed array element */

        /* Get the info about the chunk for the index */
        if (H5FA_get(fa, idx, &elmt) < 0)
            HGOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get chunk info")

        /* Remove raw data chunk from file if not doing SWMR writes */
        HDassert(H5F_addr_defined(elmt.addr));
        if (!(H5F_INTENT(idx_info->f) & H5F_ACC_SWMR_WRITE)) {
            H5_CHECK_OVERFLOW(elmt.nbytes, /*From: */ uint32_t, /*To: */ hsize_t);
            if (H5MF_xfree(idx_info->f, H5FD_MEM_DRAW, elmt.addr, (hsize_t)elmt.nbytes) < 0)
                HGOTO_ERROR(H5E_DATASET, H5E_CANTFREE, FAIL, "unable to free chunk")
        } /* end if */

        /* Reset the info about the chunk for the index */
        elmt.addr        = HADDR_UNDEF;
        elmt.nbytes      = 0;
        elmt.filter_mask = 0;
        if (H5FA_set(fa, idx, &elmt) < 0)
            HGOTO_ERROR(H5E_DATASET, H5E_CANTSET, FAIL, "unable to reset chunk info")
    } /* end if */
    else {
        haddr_t addr = HADDR_UNDEF; /* Chunk address */

        /* Get the address of the chunk for the index */
        if (H5FA_get(fa, idx, &addr) < 0)
            HGOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't get chunk address")

        /* Remove raw data chunk from file if not doing SWMR writes */
        HDassert(H5F_addr_defined(addr));
        if (!(H5F_INTENT(idx_info->f) & H5F_ACC_SWMR_WRITE)) {
            H5_CHECK_OVERFLOW(idx_info->layout->size, /*From: */ uint32_t, /*To: */ hsize_t);
            if (H5MF_xfree(idx_info->f, H5FD_MEM_DRAW, addr, (hsize_t)idx_info->layout->size) < 0)
                HGOTO_ERROR(H5E_DATASET, H5E_CANTFREE, FAIL, "unable to free chunk")
        } /* end if */

        /* Reset the address of the chunk for the index */
        addr = HADDR_UNDEF;
        if (H5FA_set(fa, idx, &addr) < 0)
            HGOTO_ERROR(H5E_DATASET, H5E_CANTSET, FAIL, "unable to reset chunk address")
    } /* end else */

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5D__farray_idx_remove() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_idx_delete_cb
 *
 * Purpose:     Delete space for chunk in file
 *
 * Return:      Success:    Non-negative
 *              Failure:    negative
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static int
H5D__farray_idx_delete_cb(const H5D_chunk_rec_t *chunk_rec, void *_udata)
{
    H5F_t *f         = (H5F_t *)_udata; /* User data for callback */
    int    ret_value = H5_ITER_CONT;    /* Return value */

    FUNC_ENTER_STATIC

    /* Sanity checks */
    HDassert(chunk_rec);
    HDassert(H5F_addr_defined(chunk_rec->chunk_addr));
    HDassert(chunk_rec->nbytes > 0);
    HDassert(f);

    /* Remove raw data chunk from file */
    H5_CHECK_OVERFLOW(chunk_rec->nbytes, /*From: */ uint32_t, /*To: */ hsize_t);
    if (H5MF_xfree(f, H5FD_MEM_DRAW, chunk_rec->chunk_addr, (hsize_t)chunk_rec->nbytes) < 0)
        HGOTO_ERROR(H5E_DATASET, H5E_CANTFREE, H5_ITER_ERROR, "unable to free chunk")

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5D__farray_idx_delete_cb() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_idx_delete
 *
 * Purpose:     Delete index and raw data storage for entire dataset
 *              (i.e. all chunks)
 *
 * Return:      Success:    Non-negative
 *              Failure:    negative
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_idx_delete(const H5D_chk_idx_info_t *idx_info)
{
    herr_t ret_value = SUCCEED; /* Return value */

    FUNC_ENTER_STATIC

    /* Sanity checks */
    HDassert(idx_info);
    HDassert(idx_info->f);
    HDassert(idx_info->pline);
    HDassert(idx_info->layout);
    HDassert(idx_info->storage);

    /* Check if the index data structure has been allocated */
    if (H5F_addr_defined(idx_info->storage->idx_addr)) {
        H5D_farray_ctx_ud_t ctx_udata; /* User data for fixed array open call */

        /* Iterate over the chunk addresses in the fixed array, deleting each chunk */
        if (H5D__farray_idx_iterate(idx_info, H5D__farray_idx_delete_cb, idx_info->f) < 0)
            HGOTO_ERROR(H5E_DATASET, H5E_BADITER, FAIL, "unable to iterate over chunk addresses")

        /* Close fixed array */
        if (H5FA_close(idx_info->storage->u.farray.fa) < 0)
            HGOTO_ERROR(H5E_DATASET, H5E_CANTCLOSEOBJ, FAIL, "unable to close fixed array")
        idx_info->storage->u.farray.fa = NULL;

        /* Set up the user data */
        ctx_udata.f          = idx_info->f;
        ctx_udata.chunk_size = idx_info->layout->size;

        /* Delete fixed array */
        if (H5FA_delete(idx_info->f, idx_info->storage->idx_addr, &ctx_udata) < 0)
            HGOTO_ERROR(H5E_DATASET, H5E_CANTDELETE, FAIL, "unable to delete chunk fixed array")
        idx_info->storage->idx_addr = HADDR_UNDEF;
    } /* end if */
    else
        HDassert(NULL == idx_info->storage->u.farray.fa);

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5D__farray_idx_delete() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_idx_copy_setup
 *
 * Purpose:     Set up any necessary information for copying chunks
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_idx_copy_setup(const H5D_chk_idx_info_t *idx_info_src, const H5D_chk_idx_info_t *idx_info_dst)
{
    herr_t ret_value = SUCCEED; /* Return value */

    FUNC_ENTER_STATIC

    /* Check args */
    HDassert(idx_info_src);
    HDassert(idx_info_src->f);
    HDassert(idx_info_src->pline);
    HDassert(idx_info_src->layout);
    HDassert(idx_info_src->storage);
    HDassert(idx_info_dst);
    HDassert(idx_info_dst->f);
    HDassert(idx_info_dst->pline);
    HDassert(idx_info_dst->layout);
    HDassert(idx_info_dst->storage);
    HDassert(!H5F_addr_defined(idx_info_dst->storage->idx_addr));

    /* Check if the source fixed array is open yet */
    if (NULL == idx_info_src->storage->u.farray.fa)
        /* Open the fixed array in file */
        if (H5D__farray_idx_open(idx_info_src) < 0)
            HGOTO_ERROR(H5E_DATASET, H5E_CANTOPENOBJ, FAIL, "can't open fixed array")

    /* Set copied metadata tag */
    H5_BEGIN_TAG(H5AC__COPIED_TAG);

    /* Create the fixed array that describes chunked storage in the dest. file */
    if (H5D__farray_idx_create(idx_info_dst) < 0)
        HGOTO_ERROR(H5E_DATASET, H5E_CANTINIT, FAIL, "unable to initialize chunked storage")
    HDassert(H5F_addr_defined(idx_info_dst->storage->idx_addr));

    /* Reset metadata tag */
    H5_END_TAG

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5D__farray_idx_copy_setup() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_idx_copy_shutdown
 *
 * Purpose:     Shutdown any information from copying chunks
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_idx_copy_shutdown(H5O_storage_chunk_t *storage_src, H5O_storage_chunk_t *storage_dst)
{
    herr_t ret_value = SUCCEED; /* Return value */

    FUNC_ENTER_STATIC

    /* Check args */
    HDassert(storage_src);
    HDassert(storage_src->u.farray.fa);
    HDassert(storage_dst);
    HDassert(storage_dst->u.farray.fa);

    /* Close fixed arrays */
    if (H5FA_close(storage_src->u.farray.fa) < 0)
        HGOTO_ERROR(H5E_DATASET, H5E_CANTCLOSEOBJ, FAIL, "unable to close fixed array")
    storage_src->u.farray.fa = NULL;
    if (H5FA_close(storage_dst->u.farray.fa) < 0)
        HGOTO_ERROR(H5E_DATASET, H5E_CANTCLOSEOBJ, FAIL, "unable to close fixed array")
    storage_dst->u.farray.fa = NULL;

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5D__farray_idx_copy_shutdown() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_idx_size
 *
 * Purpose:     Retrieve the amount of index storage for chunked dataset
 *
 * Return:      Success:        Non-negative
 *              Failure:        negative
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_idx_size(const H5D_chk_idx_info_t *idx_info, hsize_t *index_size)
{
    H5FA_t     *fa;                  /* Pointer to fixed array structure */
    H5FA_stat_t fa_stat;             /* Fixed array statistics */
    herr_t      ret_value = SUCCEED; /* Return value */

    FUNC_ENTER_STATIC

    /* Check args */
    HDassert(idx_info);
    HDassert(idx_info->f);
    HDassert(idx_info->pline);
    HDassert(idx_info->layout);
    HDassert(idx_info->storage);
    HDassert(H5F_addr_defined(idx_info->storage->idx_addr));
    HDassert(index_size);

    /* Open the fixed array in file */
    if (H5D__farray_idx_open(idx_info) < 0)
        HGOTO_ERROR(H5E_DATASET, H5E_CANTOPENOBJ, FAIL, "can't open fixed array")

    /* Set convenience pointer to fixed array structure */
    fa = idx_info->storage->u.farray.fa;

    /* Get the fixed array statistics */
    if (H5FA_get_stats(fa, &fa_stat) < 0)
        HGOTO_ERROR(H5E_DATASET, H5E_CANTGET, FAIL, "can't query fixed array statistics")

    *index_size = fa_stat.hdr_size;
    *index_size += fa_stat.dblk_size;

done:
    if (idx_info->storage->u.farray.fa) {
        if (H5FA_close(idx_info->storage->u.farray.fa) < 0)
            HGOTO_ERROR(H5E_DATASET, H5E_CANTCLOSEOBJ, FAIL, "unable to close fixed array")
        idx_info->storage->u.farray.fa = NULL;
    } /* end if */

    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5D__farray_idx_size() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_idx_reset
 *
 * Purpose:     Reset indexing information.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_idx_reset(H5O_storage_chunk_t *storage, hbool_t reset_addr)
{
    FUNC_ENTER_STATIC_NOERR

    /* Check args */
    HDassert(storage);

    /* Reset index info */
    if (reset_addr)
        storage->idx_addr = HADDR_UNDEF;
    storage->u.farray.fa = NULL;

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5D__farray_idx_reset() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_idx_dump
 *
 * Purpose:     Dump indexing information to a stream.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_idx_dump(const H5O_storage_chunk_t *storage, FILE *stream)
{
    FUNC_ENTER_STATIC_NOERR

    /* Check args */
    HDassert(storage);
    HDassert(stream);

    HDfprintf(stream, "    Address: %" PRIuHADDR "\n", storage->idx_addr);

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5D__farray_idx_dump() */

/*-------------------------------------------------------------------------
 * Function:    H5D__farray_idx_dest
 *
 * Purpose:     Release indexing information in memory.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Vailin Choi
 *              Thursday, April 30, 2009
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5D__farray_idx_dest(const H5D_chk_idx_info_t *idx_info)
{
    herr_t ret_value = SUCCEED; /* Return value */

    FUNC_ENTER_STATIC

    /* Check args */
    HDassert(idx_info);
    HDassert(idx_info->f);
    HDassert(idx_info->storage);

    /* Check if the fixed array is open */
    if (idx_info->storage->u.farray.fa) {

        /* Patch the top level file pointer contained in fa if needed */
        if (H5FA_patch_file(idx_info->storage->u.farray.fa, idx_info->f) < 0)
            HGOTO_ERROR(H5E_DATASET, H5E_CANTOPENOBJ, FAIL, "can't patch fixed array file pointer")

        /* Close fixed array */
        if (H5FA_close(idx_info->storage->u.farray.fa) < 0)
            HGOTO_ERROR(H5E_DATASET, H5E_CANTCLOSEOBJ, FAIL, "unable to close fixed array")
        idx_info->storage->u.farray.fa = NULL;
    } /* end if */

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5D__farray_idx_dest() */
