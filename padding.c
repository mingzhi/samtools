#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "kstring.h"
#include "sam_header.h"
#include "sam.h"
#include "bam.h"

static void replace_cigar(bam1_t *b, int n, uint32_t *cigar)
{
	if (n != b->core.n_cigar) {
		int o = b->core.l_qname + b->core.n_cigar * 4;
		if (b->data_len + (n - b->core.n_cigar) * 4 > b->m_data) {
			b->m_data = b->data_len + (n - b->core.n_cigar) * 4;
			kroundup32(b->m_data);
			b->data = (uint8_t*)realloc(b->data, b->m_data);
		}
		memmove(b->data + b->core.l_qname + n * 4, b->data + o, b->data_len - o);
		memcpy(b->data + b->core.l_qname, cigar, n * 4);
		b->data_len += (n - b->core.n_cigar) * 4;
		b->core.n_cigar = n;
	} else memcpy(b->data + b->core.l_qname, cigar, n * 4);
}

#define write_cigar(_c, _n, _m, _v) do { \
		if (_n == _m) { \
			_m = _m? _m<<1 : 4; \
			_c = (uint32_t*)realloc(_c, _m * 4); \
		} \
		_c[_n++] = (_v); \
	} while (0)

static void unpad_seq(bam1_t *b, kstring_t *s)
{
	int k, j, i;
	int length;
	uint32_t *cigar = bam1_cigar(b);
	uint8_t *seq = bam1_seq(b);
	// b->core.l_qseq gives length of the SEQ entry (including soft clips, S)
	// We need the padded length after alignment from the CIGAR (excluding
	// soft clips S, but including pads from CIGAR D operations)
	length = 0;
	for (k = 0; k < b->core.n_cigar; ++k) {
		int op, ol;
		op= bam_cigar_op(cigar[k]);
		ol = bam_cigar_oplen(cigar[k]);
		if (op == BAM_CMATCH || op == BAM_CEQUAL || op == BAM_CDIFF || op == BAM_CDEL)
			length += ol;
	}
	ks_resize(s, length);
	for (k = 0, s->l = 0, j = 0; k < b->core.n_cigar; ++k) {
		int op, ol;
		op = bam_cigar_op(cigar[k]);
		ol = bam_cigar_oplen(cigar[k]);
		if (op == BAM_CMATCH || op == BAM_CEQUAL || op == BAM_CDIFF) {
			for (i = 0; i < ol; ++i, ++j) s->s[s->l++] = bam1_seqi(seq, j);
		} else if (op == BAM_CSOFT_CLIP) {
			j += ol;
		} else if (op == BAM_CHARD_CLIP) {
			/* do nothing */
		} else if (op == BAM_CDEL) {
			for (i = 0; i < ol; ++i) s->s[s->l++] = 0;
                } else {
			fprintf(stderr, "[depad] ERROR: Didn't expect CIGAR op %c in read %s\n", BAM_CIGAR_STR[op], bam1_qname(b));
                        assert(-1);
		}
	}
	assert(length == s->l);
}

int bam_pad2unpad(samfile_t *in, samfile_t *out)
{
	bam_header_t *h;
	bam1_t *b;
	kstring_t r, q;
	int r_tid = -1;
	uint32_t *cigar2 = 0;
	int ret = 0, n2 = 0, m2 = 0, *posmap = 0;

	b = bam_init1();
	r.l = r.m = q.l = q.m = 0; r.s = q.s = 0;
	int read_ret;
	h = in->header;
	while ((read_ret = samread(in, b)) >= 0) { // read one alignment from `in'
		uint32_t *cigar = bam1_cigar(b);
		n2 = 0;
		if (b->core.pos == 0 && b->core.tid >= 0 && strcmp(bam1_qname(b), h->target_name[b->core.tid]) == 0) {
			int i, k;
			/*
			fprintf(stderr, "[depad] Found embedded reference %s\n", bam1_qname(b));
			*/
			r_tid = b->core.tid;
			unpad_seq(b, &r);
			if (h->target_len[r_tid] != r.l) {
				fprintf(stderr, "[depad] ERROR: (Padded) length of %s is %i in BAM header, but %ld in embedded reference\n", bam1_qname(b), h->target_len[r_tid], r.l);
				return -1;
			}
			write_cigar(cigar2, n2, m2, bam_cigar_gen(b->core.l_qseq, BAM_CMATCH));
			replace_cigar(b, n2, cigar2);
			posmap = realloc(posmap, r.m * sizeof(int));
			for (i = k = 0; i < r.l; ++i) {
				posmap[i] = k;
				if (r.s[i]) ++k;
			}
		} else if (b->core.n_cigar > 0) {
			int i, k, op;
			if (b->core.tid < 0) {
				fprintf(stderr, "[depad] ERROR: Read %s has CIGAR but no RNAME\n", bam1_qname(b));
				return -1;
			} else if (b->core.tid != r_tid) {
				fprintf(stderr, "[depad] ERROR: Missing %s embedded reference sequence\n", h->target_name[b->core.tid]);
				return -1;
			}
			unpad_seq(b, &q);
			if (bam_cigar_op(cigar[0]) == BAM_CSOFT_CLIP) {
				write_cigar(cigar2, n2, m2, cigar[0]);
			} else if (bam_cigar_op(cigar[0]) == BAM_CHARD_CLIP) {
				write_cigar(cigar2, n2, m2, cigar[0]);
				if (b->core.n_cigar > 2 && bam_cigar_op(cigar[1]) == BAM_CSOFT_CLIP) {
					write_cigar(cigar2, n2, m2, cigar[1]);
				}
			}
			/* Determine CIGAR operator for each base in the aligned read */
			for (i = 0, k = b->core.pos; i < q.l; ++i, ++k)
				q.s[i] = q.s[i]? (r.s[k]? BAM_CMATCH : BAM_CINS) : (r.s[k]? BAM_CDEL : BAM_CPAD);
			/* Include any pads if starts with an insert */
			if (q.s[0] == BAM_CINS) {
				for (k = 0; k+1 < b->core.pos && !r.s[b->core.pos - k - 1]; ++k);
				if (k) write_cigar(cigar2, n2, m2, bam_cigar_gen(k, BAM_CPAD));
			}
			/* Count consecutive CIGAR operators to turn into a CIGAR string */
			for (i = k = 1, op = q.s[0]; i < q.l; ++i) {
				if (op != q.s[i]) {
					write_cigar(cigar2, n2, m2, bam_cigar_gen(k, op));
					op = q.s[i]; k = 1;
				} else ++k;
			}
			write_cigar(cigar2, n2, m2, bam_cigar_gen(k, op));
			if (bam_cigar_op(cigar[b->core.n_cigar-1]) == BAM_CSOFT_CLIP) {
				write_cigar(cigar2, n2, m2, cigar[b->core.n_cigar-1]);
                        } else if (bam_cigar_op(cigar[b->core.n_cigar-1]) == BAM_CHARD_CLIP) {
				if (b->core.n_cigar > 2 && bam_cigar_op(cigar[b->core.n_cigar-2]) == BAM_CSOFT_CLIP) {
					write_cigar(cigar2, n2, m2, cigar[b->core.n_cigar-2]);
			  	}
				write_cigar(cigar2, n2, m2, cigar[b->core.n_cigar-1]);
			}
			/* Remove redundant P operators between M/X/=/D operators, e.g. 5M2P10M -> 15M */
			int pre_op, post_op;
			for (i = 2; i < n2; ++i)
				if (bam_cigar_op(cigar2[i-1]) == BAM_CPAD) {
					pre_op = bam_cigar_op(cigar2[i-2]);
					post_op = bam_cigar_op(cigar2[i]);
					/* Note don't need to check for X/= as code above will use M only */
					if ((pre_op == BAM_CMATCH || pre_op == BAM_CDEL) && (post_op == BAM_CMATCH || post_op == BAM_CDEL)) {
						/* This is a redundant P operator */
						cigar2[i-1] = 0; // i.e. 0M
						/* If had same operator either side, combine them in post_op */
						if (pre_op == post_op) {
							/* If CIGAR M, could treat as simple integers since BAM_CMATCH is zero*/
							cigar2[i] = bam_cigar_gen(bam_cigar_oplen(cigar2[i-2]) + bam_cigar_oplen(cigar2[i]), post_op);
							cigar2[i-2] = 0; // i.e. 0M
						}
					}
				}
			/* Remove the zero'd operators (0M) */
			for (i = k = 0; i < n2; ++i)
				if (cigar2[i]) cigar2[k++] = cigar2[i];
			n2 = k;
			replace_cigar(b, n2, cigar2);
			b->core.pos = posmap[b->core.pos];
		}
		samwrite(out, b);
	}
	if (read_ret < -1) {
		fprintf(stderr, "[depad] truncated file.\n");
		ret = 1;
	}
	free(r.s); free(q.s); free(posmap);
	bam_destroy1(b);
	return ret;
}

static int usage(int is_long_help);

int main_pad2unpad(int argc, char *argv[])
{
	samfile_t *in = 0, *out = 0;
	int c, is_bamin = 1, compress_level = -1, is_bamout = 1, is_long_help = 0;
	char in_mode[5], out_mode[5], *fn_out = 0, *fn_list = 0;
        int ret=0;

	/* parse command-line options */
	strcpy(in_mode, "r"); strcpy(out_mode, "w");
	while ((c = getopt(argc, argv, "Sso:u1?")) >= 0) {
		switch (c) {
		case 'S': is_bamin = 0; break;
		case 's': assert(compress_level == -1); is_bamout = 0; break;
		case 'o': fn_out = strdup(optarg); break;
		case 'u': assert(is_bamout == 1); compress_level = 0; break;
		case '1': assert(is_bamout == 1); compress_level = 1; break;
                case '?': is_long_help = 1; break;
		default: return usage(is_long_help);
		}
        }
	if (argc == optind) return usage(is_long_help);

	if (is_bamin) strcat(in_mode, "b");
	if (is_bamout) strcat(out_mode, "b");
	strcat(out_mode, "h");
	if (compress_level >= 0) {
		char tmp[2];
		tmp[0] = compress_level + '0'; tmp[1] = '\0';
		strcat(out_mode, tmp);
	}

	// open file handlers
	if ((in = samopen(argv[optind], in_mode, fn_list)) == 0) {
		fprintf(stderr, "[depad] fail to open \"%s\" for reading.\n", argv[optind]);
		ret = 1;
		goto depad_end;
	}
	if (in->header == 0) {
		fprintf(stderr, "[depad] fail to read the header from \"%s\".\n", argv[optind]);
		ret = 1;
		goto depad_end;
	}
	/* TODO - The reference sequence lengths in the BAM + SAM headers should be updated */
	if ((out = samopen(fn_out? fn_out : "-", out_mode, in->header)) == 0) {
		fprintf(stderr, "[depad] fail to open \"%s\" for writing.\n", fn_out? fn_out : "standard output");
		ret = 1;
		goto depad_end;
	}

	// Do the depad
	ret = bam_pad2unpad(in, out);

depad_end:
	// close files, free and return
	free(fn_list); free(fn_out);
	samclose(in);
	samclose(out);
	return ret;
}

static int usage(int is_long_help)
{
	fprintf(stderr, "\n");
	fprintf(stderr, "Usage:   samtools depad <in.bam>\n\n");
	fprintf(stderr, "Options: -s       output is SAM (default is BAM)\n");
	fprintf(stderr, "         -S       input is SAM (default is BAM)\n");
	fprintf(stderr, "         -u       uncompressed BAM output (can't use with -s)\n");
	fprintf(stderr, "         -1       fast compression BAM output (can't use with -s)\n");
        //TODO - These are the arguments I think make sense to support:
	//fprintf(stderr, "         -@ INT   number of BAM compression threads [0]\n");
	//fprintf(stderr, "         -T FILE  reference sequence file (force -S) [null]\n");
	fprintf(stderr, "         -o FILE  output file name [stdout]\n");
	fprintf(stderr, "         -?       longer help\n");
	fprintf(stderr, "\n");
	if (is_long_help)
		fprintf(stderr, "Notes:\n\
\n\
  1. Requires embedded reference sequences (before the reads for that reference).\n\
\n\
  2. The input padded alignment read's CIGAR strings must not use P or I operators.\n\
\n");
        return 1;
}
