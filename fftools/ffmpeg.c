/*
 * Copyright (c) 2000-2003 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * multimedia converter based on the FFmpeg libraries
 */

#include "config.h"
#include <ctype.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <libvmaf.h>

#if HAVE_IO_H
#include <io.h>
#endif
#if HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"
#include "libswresample/swresample.h"
#include "libavutil/opt.h"
#include "libavutil/channel_layout.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/fifo.h"
#include "libavutil/hwcontext.h"
#include "libavutil/internal.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/dict.h"
#include "libavutil/display.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/avstring.h"
#include "libavutil/libm.h"
#include "libavutil/imgutils.h"
#include "libavutil/timestamp.h"
#include "libavutil/bprint.h"
#include "libavutil/time.h"
#include "libavutil/thread.h"
#include "libavutil/threadmessage.h"
#include "libavcodec/mathops.h"
#include "libavformat/os_support.h"

# include "libavfilter/avfilter.h"
# include "libavfilter/buffersrc.h"
# include "libavfilter/buffersink.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
//#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <sys/time.h>

#if HAVE_SYS_RESOURCE_H
#include <sys/time.h>
#include <sys/types.h>
#include <sys/resource.h>
#elif HAVE_GETPROCESSTIMES
#include <windows.h>
#endif
#if HAVE_GETPROCESSMEMORYINFO
#include <windows.h>
#include <psapi.h>
#endif
#if HAVE_SETCONSOLECTRLHANDLER
#include <windows.h>
#endif


#if HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#if HAVE_TERMIOS_H
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <termios.h>
#elif HAVE_KBHIT
#include <conio.h>
#endif

#include <time.h>

#include "ffmpeg.h"
#include "cmdutils.h"


#include "libavutil/avassert.h"

const char program_name[] = "ffmpeg";
const int program_birth_year = 2000;

static FILE *vstats_file;

const char *const forced_keyframes_const_names[] = {
    "n",
    "n_forced",
    "prev_forced_n",
    "prev_forced_t",
    "t",
    NULL
};

typedef struct BenchmarkTimeStamps {
    int64_t real_usec;
    int64_t user_usec;
    int64_t sys_usec;
} BenchmarkTimeStamps;

static void do_video_stats(OutputStream *ost, int frame_size);
static BenchmarkTimeStamps get_benchmark_time_stamps(void);
static int64_t getmaxrss(void);
static int ifilter_has_all_input_formats(FilterGraph *fg);

static int run_as_daemon  = 0;
static int nb_frames_dup = 0;
static unsigned dup_warning = 1000;
static int nb_frames_drop = 0;
static int64_t decode_error_stat[2];

static int want_sdp = 1;

static BenchmarkTimeStamps current_time;
AVIOContext *progress_avio = NULL;

static uint8_t *subtitle_out;

InputStream **input_streams = NULL;
int        nb_input_streams = 0;
InputFile   **input_files   = NULL;
int        nb_input_files   = 0;

OutputStream **output_streams = NULL;
int         nb_output_streams = 0;
OutputFile   **output_files   = NULL;
int         nb_output_files   = 0;

FilterGraph **filtergraphs;
int        nb_filtergraphs;

#if HAVE_TERMIOS_H

/* init terminal so that we can grab keys */
static struct termios oldtty;
static int restore_tty;
#endif

#if HAVE_THREADS
static void free_input_threads(void);
#endif

/* sub2video hack:
   Convert subtitles to video with alpha to insert them in filter graphs.
   This is a temporary solution until libavfilter gets real subtitles support.
 */

static int sub2video_get_blank_frame(InputStream *ist)
{
    int ret;
    AVFrame *frame = ist->sub2video.frame;

    av_frame_unref(frame);
    ist->sub2video.frame->width  = ist->dec_ctx->width  ? ist->dec_ctx->width  : ist->sub2video.w;
    ist->sub2video.frame->height = ist->dec_ctx->height ? ist->dec_ctx->height : ist->sub2video.h;
    ist->sub2video.frame->format = AV_PIX_FMT_RGB32;
    if ((ret = av_frame_get_buffer(frame, 32)) < 0)
        return ret;
    memset(frame->data[0], 0, frame->height * frame->linesize[0]);
    return 0;
}

static void sub2video_copy_rect(uint8_t *dst, int dst_linesize, int w, int h,
                                AVSubtitleRect *r)
{
    uint32_t *pal, *dst2;
    uint8_t *src, *src2;
    int x, y;

    if (r->type != SUBTITLE_BITMAP) {
        av_log(NULL, AV_LOG_WARNING, "sub2video: non-bitmap subtitle\n");
        return;
    }
    if (r->x < 0 || r->x + r->w > w || r->y < 0 || r->y + r->h > h) {
        av_log(NULL, AV_LOG_WARNING, "sub2video: rectangle (%d %d %d %d) overflowing %d %d\n",
            r->x, r->y, r->w, r->h, w, h
        );
        return;
    }

    dst += r->y * dst_linesize + r->x * 4;
    src = r->data[0];
    pal = (uint32_t *)r->data[1];
    for (y = 0; y < r->h; y++) {
        dst2 = (uint32_t *)dst;
        src2 = src;
        for (x = 0; x < r->w; x++)
            *(dst2++) = pal[*(src2++)];
        dst += dst_linesize;
        src += r->linesize[0];
    }
}

static void sub2video_push_ref(InputStream *ist, int64_t pts)
{
    AVFrame *frame = ist->sub2video.frame;
    int i;
    int ret;

    av_assert1(frame->data[0]);
    ist->sub2video.last_pts = frame->pts = pts;
    for (i = 0; i < ist->nb_filters; i++) {
        ret = av_buffersrc_add_frame_flags(ist->filters[i]->filter, frame,
                                           AV_BUFFERSRC_FLAG_KEEP_REF |
                                           AV_BUFFERSRC_FLAG_PUSH);
        if (ret != AVERROR_EOF && ret < 0)
            av_log(NULL, AV_LOG_WARNING, "Error while add the frame to buffer source(%s).\n",
                   av_err2str(ret));
    }
}

void sub2video_update(InputStream *ist, AVSubtitle *sub)
{
    AVFrame *frame = ist->sub2video.frame;
    int8_t *dst;
    int     dst_linesize;
    int num_rects, i;
    int64_t pts, end_pts;

    if (!frame)
        return;
    if (sub) {
        pts       = av_rescale_q(sub->pts + sub->start_display_time * 1000LL,
                                 AV_TIME_BASE_Q, ist->st->time_base);
        end_pts   = av_rescale_q(sub->pts + sub->end_display_time   * 1000LL,
                                 AV_TIME_BASE_Q, ist->st->time_base);
        num_rects = sub->num_rects;
    } else {
        pts       = ist->sub2video.end_pts;
        end_pts   = INT64_MAX;
        num_rects = 0;
    }
    if (sub2video_get_blank_frame(ist) < 0) {
        av_log(ist->dec_ctx, AV_LOG_ERROR,
               "Impossible to get a blank canvas.\n");
        return;
    }
    dst          = frame->data    [0];
    dst_linesize = frame->linesize[0];
    for (i = 0; i < num_rects; i++)
        sub2video_copy_rect(dst, dst_linesize, frame->width, frame->height, sub->rects[i]);
    sub2video_push_ref(ist, pts);
    ist->sub2video.end_pts = end_pts;
}

static void sub2video_heartbeat(InputStream *ist, int64_t pts)
{
    InputFile *infile = input_files[ist->file_index];
    int i, j, nb_reqs;
    int64_t pts2;

    /* When a frame is read from a file, examine all sub2video streams in
       the same file and send the sub2video frame again. Otherwise, decoded
       video frames could be accumulating in the filter graph while a filter
       (possibly overlay) is desperately waiting for a subtitle frame. */
    for (i = 0; i < infile->nb_streams; i++) {
        InputStream *ist2 = input_streams[infile->ist_index + i];
        if (!ist2->sub2video.frame)
            continue;
        /* subtitles seem to be usually muxed ahead of other streams;
           if not, subtracting a larger time here is necessary */
        pts2 = av_rescale_q(pts, ist->st->time_base, ist2->st->time_base) - 1;
        /* do not send the heartbeat frame if the subtitle is already ahead */
        if (pts2 <= ist2->sub2video.last_pts)
            continue;
        if (pts2 >= ist2->sub2video.end_pts ||
            (!ist2->sub2video.frame->data[0] && ist2->sub2video.end_pts < INT64_MAX))
            sub2video_update(ist2, NULL);
        for (j = 0, nb_reqs = 0; j < ist2->nb_filters; j++)
            nb_reqs += av_buffersrc_get_nb_failed_requests(ist2->filters[j]->filter);
        if (nb_reqs)
            sub2video_push_ref(ist2, pts2);
    }
}

static void sub2video_flush(InputStream *ist)
{
    int i;
    int ret;

    if (ist->sub2video.end_pts < INT64_MAX)
        sub2video_update(ist, NULL);
    for (i = 0; i < ist->nb_filters; i++) {
        ret = av_buffersrc_add_frame(ist->filters[i]->filter, NULL);
        if (ret != AVERROR_EOF && ret < 0)
            av_log(NULL, AV_LOG_WARNING, "Flush the frame error.\n");
    }
}

/* end of sub2video hack */

static void term_exit_sigsafe(void)
{
#if HAVE_TERMIOS_H
    if(restore_tty)
        tcsetattr (0, TCSANOW, &oldtty);
#endif
}

void term_exit(void)
{
    av_log(NULL, AV_LOG_QUIET, "%s", "");
    term_exit_sigsafe();
}

static volatile int received_sigterm = 0;
static volatile int received_nb_signals = 0;
static atomic_int transcode_init_done = ATOMIC_VAR_INIT(0);
static volatile int ffmpeg_exited = 0;
static int main_return_code = 0;

static void
sigterm_handler(int sig)
{
    int ret;
    received_sigterm = sig;
    received_nb_signals++;
    term_exit_sigsafe();
    if(received_nb_signals > 3) {
        ret = write(2/*STDERR_FILENO*/, "Received > 3 system signals, hard exiting\n",
                    strlen("Received > 3 system signals, hard exiting\n"));
        if (ret < 0) { /* Do nothing */ };
        exit(123);
    }
}

#if HAVE_SETCONSOLECTRLHANDLER
static BOOL WINAPI CtrlHandler(DWORD fdwCtrlType)
{
    av_log(NULL, AV_LOG_DEBUG, "\nReceived windows signal %ld\n", fdwCtrlType);

    switch (fdwCtrlType)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        sigterm_handler(SIGINT);
        return TRUE;

    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        sigterm_handler(SIGTERM);
        /* Basically, with these 3 events, when we return from this method the
           process is hard terminated, so stall as long as we need to
           to try and let the main thread(s) clean up and gracefully terminate
           (we have at most 5 seconds, but should be done far before that). */
        while (!ffmpeg_exited) {
            Sleep(0);
        }
        return TRUE;

    default:
        av_log(NULL, AV_LOG_ERROR, "Received unknown windows signal %ld\n", fdwCtrlType);
        return FALSE;
    }
}
#endif

void term_init(void)
{
#if HAVE_TERMIOS_H
    if (!run_as_daemon && stdin_interaction) {
        struct termios tty;
        if (tcgetattr (0, &tty) == 0) {
            oldtty = tty;
            restore_tty = 1;

            tty.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP
                             |INLCR|IGNCR|ICRNL|IXON);
            tty.c_oflag |= OPOST;
            tty.c_lflag &= ~(ECHO|ECHONL|ICANON|IEXTEN);
            tty.c_cflag &= ~(CSIZE|PARENB);
            tty.c_cflag |= CS8;
            tty.c_cc[VMIN] = 1;
            tty.c_cc[VTIME] = 0;

            tcsetattr (0, TCSANOW, &tty);
        }
        signal(SIGQUIT, sigterm_handler); /* Quit (POSIX).  */
    }
#endif

    signal(SIGINT , sigterm_handler); /* Interrupt (ANSI).    */
    signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */
#ifdef SIGXCPU
    signal(SIGXCPU, sigterm_handler);
#endif
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN); /* Broken pipe (POSIX). */
#endif
#if HAVE_SETCONSOLECTRLHANDLER
    SetConsoleCtrlHandler((PHANDLER_ROUTINE) CtrlHandler, TRUE);
#endif
}

/* read a key without blocking */
static int read_key(void)
{
    unsigned char ch;
#if HAVE_TERMIOS_H
    int n = 1;
    struct timeval tv;
    fd_set rfds;

    FD_ZERO(&rfds);
    FD_SET(0, &rfds);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    n = select(1, &rfds, NULL, NULL, &tv);
    if (n > 0) {
        n = read(0, &ch, 1);
        if (n == 1)
            return ch;

        return n;
    }
#elif HAVE_KBHIT
#    if HAVE_PEEKNAMEDPIPE
    static int is_pipe;
    static HANDLE input_handle;
    DWORD dw, nchars;
    if(!input_handle){
        input_handle = GetStdHandle(STD_INPUT_HANDLE);
        is_pipe = !GetConsoleMode(input_handle, &dw);
    }

    if (is_pipe) {
        /* When running under a GUI, you will end here. */
        if (!PeekNamedPipe(input_handle, NULL, 0, NULL, &nchars, NULL)) {
            // input pipe may have been closed by the program that ran ffmpeg
            return -1;
        }
        //Read it
        if(nchars != 0) {
            read(0, &ch, 1);
            return ch;
        }else{
            return -1;
        }
    }
#    endif
    if(kbhit())
        return(getch());
#endif
    return -1;
}

static int decode_interrupt_cb(void *ctx)
{
    return received_nb_signals > atomic_load(&transcode_init_done);
}

const AVIOInterruptCB int_cb = { decode_interrupt_cb, NULL };

static void ffmpeg_cleanup(int ret)
{
    int i, j;

    if (do_benchmark) {
        int maxrss = getmaxrss() / 1024;
        av_log(NULL, AV_LOG_INFO, "bench: maxrss=%ikB\n", maxrss);
    }

    for (i = 0; i < nb_filtergraphs; i++) {
        FilterGraph *fg = filtergraphs[i];
        avfilter_graph_free(&fg->graph);
        for (j = 0; j < fg->nb_inputs; j++) {
            while (av_fifo_size(fg->inputs[j]->frame_queue)) {
                AVFrame *frame;
                av_fifo_generic_read(fg->inputs[j]->frame_queue, &frame,
                                     sizeof(frame), NULL);
                av_frame_free(&frame);
            }
            av_fifo_freep(&fg->inputs[j]->frame_queue);
            if (fg->inputs[j]->ist->sub2video.sub_queue) {
                while (av_fifo_size(fg->inputs[j]->ist->sub2video.sub_queue)) {
                    AVSubtitle sub;
                    av_fifo_generic_read(fg->inputs[j]->ist->sub2video.sub_queue,
                                         &sub, sizeof(sub), NULL);
                    avsubtitle_free(&sub);
                }
                av_fifo_freep(&fg->inputs[j]->ist->sub2video.sub_queue);
            }
            av_buffer_unref(&fg->inputs[j]->hw_frames_ctx);
            av_freep(&fg->inputs[j]->name);
            av_freep(&fg->inputs[j]);
        }
        av_freep(&fg->inputs);
        for (j = 0; j < fg->nb_outputs; j++) {
            av_freep(&fg->outputs[j]->name);
            av_freep(&fg->outputs[j]->formats);
            av_freep(&fg->outputs[j]->channel_layouts);
            av_freep(&fg->outputs[j]->sample_rates);
            av_freep(&fg->outputs[j]);
        }
        av_freep(&fg->outputs);
        av_freep(&fg->graph_desc);

        av_freep(&filtergraphs[i]);
    }
    av_freep(&filtergraphs);

    av_freep(&subtitle_out);

    /* close files */
    for (i = 0; i < nb_output_files; i++) {
        OutputFile *of = output_files[i];
        AVFormatContext *s;
        if (!of)
            continue;
        s = of->ctx;
        if (s && s->oformat && !(s->oformat->flags & AVFMT_NOFILE))
            avio_closep(&s->pb);
        avformat_free_context(s);
        av_dict_free(&of->opts);

        av_freep(&output_files[i]);
    }
    for (i = 0; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];

        if (!ost)
            continue;

        for (j = 0; j < ost->nb_bitstream_filters; j++)
            av_bsf_free(&ost->bsf_ctx[j]);
        av_freep(&ost->bsf_ctx);

        av_frame_free(&ost->filtered_frame);
        av_frame_free(&ost->last_frame);
        av_dict_free(&ost->encoder_opts);

        av_freep(&ost->forced_keyframes);
        av_expr_free(ost->forced_keyframes_pexpr);
        av_freep(&ost->avfilter);
        av_freep(&ost->logfile_prefix);

        av_freep(&ost->audio_channels_map);
        ost->audio_channels_mapped = 0;

        av_dict_free(&ost->sws_dict);
        av_dict_free(&ost->swr_opts);

        avcodec_free_context(&ost->enc_ctx);
        avcodec_parameters_free(&ost->ref_par);

        if (ost->muxing_queue) {
            while (av_fifo_size(ost->muxing_queue)) {
                AVPacket pkt;
                av_fifo_generic_read(ost->muxing_queue, &pkt, sizeof(pkt), NULL);
                av_packet_unref(&pkt);
            }
            av_fifo_freep(&ost->muxing_queue);
        }

        av_freep(&output_streams[i]);
    }
#if HAVE_THREADS
    free_input_threads();
#endif
    for (i = 0; i < nb_input_files; i++) {
        avformat_close_input(&input_files[i]->ctx);
        av_freep(&input_files[i]);
    }
    for (i = 0; i < nb_input_streams; i++) {
        InputStream *ist = input_streams[i];

        av_frame_free(&ist->decoded_frame);
        av_frame_free(&ist->filter_frame);
        av_dict_free(&ist->decoder_opts);
        avsubtitle_free(&ist->prev_sub.subtitle);
        av_frame_free(&ist->sub2video.frame);
        av_freep(&ist->filters);
        av_freep(&ist->hwaccel_device);
        av_freep(&ist->dts_buffer);

        avcodec_free_context(&ist->dec_ctx);

        av_freep(&input_streams[i]);
    }

    if (vstats_file) {
        if (fclose(vstats_file))
            av_log(NULL, AV_LOG_ERROR,
                   "Error closing vstats file, loss of information possible: %s\n",
                   av_err2str(AVERROR(errno)));
    }
    av_freep(&vstats_filename);

    av_freep(&input_streams);
    av_freep(&input_files);
    av_freep(&output_streams);
    av_freep(&output_files);

    uninit_opts();

    avformat_network_deinit();

    if (received_sigterm) {
        av_log(NULL, AV_LOG_INFO, "Exiting normally, received signal %d.\n",
               (int) received_sigterm);
    } else if (ret && atomic_load(&transcode_init_done)) {
        av_log(NULL, AV_LOG_INFO, "Conversion failed!\n");
    }
    term_exit();
    ffmpeg_exited = 1;
}

void remove_avoptions(AVDictionary **a, AVDictionary *b)
{
    AVDictionaryEntry *t = NULL;

    while ((t = av_dict_get(b, "", t, AV_DICT_IGNORE_SUFFIX))) {
        av_dict_set(a, t->key, NULL, AV_DICT_MATCH_CASE);
    }
}

void assert_avoptions(AVDictionary *m)
{
    AVDictionaryEntry *t;
    if ((t = av_dict_get(m, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_FATAL, "Option %s not found.\n", t->key);
        exit_program(1);
    }
}

static void abort_codec_experimental(AVCodec *c, int encoder)
{
    exit_program(1);
}

static void update_benchmark(const char *fmt, ...)
{
    if (do_benchmark_all) {
        BenchmarkTimeStamps t = get_benchmark_time_stamps();
        va_list va;
        char buf[1024];

        if (fmt) {
            va_start(va, fmt);
            vsnprintf(buf, sizeof(buf), fmt, va);
            va_end(va);
            av_log(NULL, AV_LOG_INFO,
                   "bench: %8" PRIu64 " user %8" PRIu64 " sys %8" PRIu64 " real %s \n",
                   t.user_usec - current_time.user_usec,
                   t.sys_usec - current_time.sys_usec,
                   t.real_usec - current_time.real_usec, buf);
        }
        current_time = t;
    }
}

static void close_all_output_streams(OutputStream *ost, OSTFinished this_stream, OSTFinished others)
{
    int i;
    for (i = 0; i < nb_output_streams; i++) {
        OutputStream *ost2 = output_streams[i];
        ost2->finished |= ost == ost2 ? this_stream : others;
    }
}

static void write_packet(OutputFile *of, AVPacket *pkt, OutputStream *ost, int unqueue)
{
    AVFormatContext *s = of->ctx;
    AVStream *st = ost->st;
    int ret;

    /*
     * Audio encoders may split the packets --  #frames in != #packets out.
     * But there is no reordering, so we can limit the number of output packets
     * by simply dropping them here.
     * Counting encoded video frames needs to be done separately because of
     * reordering, see do_video_out().
     * Do not count the packet when unqueued because it has been counted when queued.
     */
    if (!(st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && ost->encoding_needed) && !unqueue) {
        if (ost->frame_number >= ost->max_frames) {
            av_packet_unref(pkt);
            return;
        }
        ost->frame_number++;
    }

    if (!of->header_written) {
        AVPacket tmp_pkt = {0};
        /* the muxer is not initialized yet, buffer the packet */
        if (!av_fifo_space(ost->muxing_queue)) {
            int new_size = FFMIN(2 * av_fifo_size(ost->muxing_queue),
                                 ost->max_muxing_queue_size);
            if (new_size <= av_fifo_size(ost->muxing_queue)) {
                av_log(NULL, AV_LOG_ERROR,
                       "Too many packets buffered for output stream %d:%d.\n",
                       ost->file_index, ost->st->index);
                exit_program(1);
            }
            ret = av_fifo_realloc2(ost->muxing_queue, new_size);
            if (ret < 0)
                exit_program(1);
        }
        ret = av_packet_make_refcounted(pkt);
        if (ret < 0)
            exit_program(1);
        av_packet_move_ref(&tmp_pkt, pkt);
        av_fifo_generic_write(ost->muxing_queue, &tmp_pkt, sizeof(tmp_pkt), NULL);
        return;
    }

    if ((st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_sync_method == VSYNC_DROP) ||
        (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_sync_method < 0))
        pkt->pts = pkt->dts = AV_NOPTS_VALUE;

    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        int i;
        uint8_t *sd = av_packet_get_side_data(pkt, AV_PKT_DATA_QUALITY_STATS,
                                              NULL);
        ost->quality = sd ? AV_RL32(sd) : -1;
        ost->pict_type = sd ? sd[4] : AV_PICTURE_TYPE_NONE;

        for (i = 0; i<FF_ARRAY_ELEMS(ost->error); i++) {
            if (sd && i < sd[5])
                ost->error[i] = AV_RL64(sd + 8 + 8*i);
            else
                ost->error[i] = -1;
        }

        if (ost->frame_rate.num && ost->is_cfr) {
            if (pkt->duration > 0)
                av_log(NULL, AV_LOG_WARNING, "Overriding packet duration by frame rate, this should not happen\n");
            pkt->duration = av_rescale_q(1, av_inv_q(ost->frame_rate),
                                         ost->mux_timebase);
        }
    }

    av_packet_rescale_ts(pkt, ost->mux_timebase, ost->st->time_base);

    if (!(s->oformat->flags & AVFMT_NOTIMESTAMPS)) {
        if (pkt->dts != AV_NOPTS_VALUE &&
            pkt->pts != AV_NOPTS_VALUE &&
            pkt->dts > pkt->pts) {
            av_log(s, AV_LOG_WARNING, "Invalid DTS: %"PRId64" PTS: %"PRId64" in output stream %d:%d, replacing by guess\n",
                   pkt->dts, pkt->pts,
                   ost->file_index, ost->st->index);
            pkt->pts =
            pkt->dts = pkt->pts + pkt->dts + ost->last_mux_dts + 1
                     - FFMIN3(pkt->pts, pkt->dts, ost->last_mux_dts + 1)
                     - FFMAX3(pkt->pts, pkt->dts, ost->last_mux_dts + 1);
        }
        if ((st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO || st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO || st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) &&
            pkt->dts != AV_NOPTS_VALUE &&
            !(st->codecpar->codec_id == AV_CODEC_ID_VP9 && ost->stream_copy) &&
            ost->last_mux_dts != AV_NOPTS_VALUE) {
            int64_t max = ost->last_mux_dts + !(s->oformat->flags & AVFMT_TS_NONSTRICT);
            if (pkt->dts < max) {
                int loglevel = max - pkt->dts > 2 || st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? AV_LOG_WARNING : AV_LOG_DEBUG;
                av_log(s, loglevel, "Non-monotonous DTS in output stream "
                       "%d:%d; previous: %"PRId64", current: %"PRId64"; ",
                       ost->file_index, ost->st->index, ost->last_mux_dts, pkt->dts);
                if (exit_on_error) {
                    av_log(NULL, AV_LOG_FATAL, "aborting.\n");
                    exit_program(1);
                }
                av_log(s, loglevel, "changing to %"PRId64". This may result "
                       "in incorrect timestamps in the output file.\n",
                       max);
                if (pkt->pts >= pkt->dts)
                    pkt->pts = FFMAX(pkt->pts, max);
                pkt->dts = max;
            }
        }
    }
    ost->last_mux_dts = pkt->dts;

    ost->data_size += pkt->size;
    ost->packets_written++;

    pkt->stream_index = ost->index;

    if (debug_ts) {
        av_log(NULL, AV_LOG_INFO, "muxer <- type:%s "
                "pkt_pts:%s pkt_pts_time:%s pkt_dts:%s pkt_dts_time:%s size:%d\n",
                av_get_media_type_string(ost->enc_ctx->codec_type),
                av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, &ost->st->time_base),
                av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, &ost->st->time_base),
                pkt->size
              );
    }

    ret = av_interleaved_write_frame(s, pkt);
    if (ret < 0) {
        print_error("av_interleaved_write_frame()", ret);
        main_return_code = 1;
        close_all_output_streams(ost, MUXER_FINISHED | ENCODER_FINISHED, ENCODER_FINISHED);
    }
    av_packet_unref(pkt);
}

static void close_output_stream(OutputStream *ost)
{
    OutputFile *of = output_files[ost->file_index];

    ost->finished |= ENCODER_FINISHED;
    if (of->shortest) {
        int64_t end = av_rescale_q(ost->sync_opts - ost->first_pts, ost->enc_ctx->time_base, AV_TIME_BASE_Q);
        of->recording_time = FFMIN(of->recording_time, end);
    }
}

/*
 * Send a single packet to the output, applying any bitstream filters
 * associated with the output stream.  This may result in any number
 * of packets actually being written, depending on what bitstream
 * filters are applied.  The supplied packet is consumed and will be
 * blank (as if newly-allocated) when this function returns.
 *
 * If eof is set, instead indicate EOF to all bitstream filters and
 * therefore flush any delayed packets to the output.  A blank packet
 * must be supplied in this case.
 */
static void output_packet(OutputFile *of, AVPacket *pkt,
                          OutputStream *ost, int eof)
{
    int ret = 0;

    /* apply the output bitstream filters, if any */
    if (ost->nb_bitstream_filters) {
        int idx;

        ret = av_bsf_send_packet(ost->bsf_ctx[0], eof ? NULL : pkt);
        if (ret < 0)
            goto finish;

        eof = 0;
        idx = 1;
        while (idx) {
            /* get a packet from the previous filter up the chain */
            ret = av_bsf_receive_packet(ost->bsf_ctx[idx - 1], pkt);
            if (ret == AVERROR(EAGAIN)) {
                ret = 0;
                idx--;
                continue;
            } else if (ret == AVERROR_EOF) {
                eof = 1;
            } else if (ret < 0)
                goto finish;

            /* send it to the next filter down the chain or to the muxer */
            if (idx < ost->nb_bitstream_filters) {
                ret = av_bsf_send_packet(ost->bsf_ctx[idx], eof ? NULL : pkt);
                if (ret < 0)
                    goto finish;
                idx++;
                eof = 0;
            } else if (eof)
                goto finish;
            else
                write_packet(of, pkt, ost, 0);
        }
    } else if (!eof)
        write_packet(of, pkt, ost, 0);

finish:
    if (ret < 0 && ret != AVERROR_EOF) {
        av_log(NULL, AV_LOG_ERROR, "Error applying bitstream filters to an output "
               "packet for stream #%d:%d.\n", ost->file_index, ost->index);
        if(exit_on_error)
            exit_program(1);
    }
}

static int check_recording_time(OutputStream *ost)
{
    OutputFile *of = output_files[ost->file_index];

    if (of->recording_time != INT64_MAX &&
        av_compare_ts(ost->sync_opts - ost->first_pts, ost->enc_ctx->time_base, of->recording_time,
                      AV_TIME_BASE_Q) >= 0) {
        close_output_stream(ost);
        return 0;
    }
    return 1;
}

static void do_audio_out(OutputFile *of, OutputStream *ost,
                         AVFrame *frame)
{
    AVCodecContext *enc = ost->enc_ctx;
    AVPacket pkt;
    int ret;

    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    if (!check_recording_time(ost))
        return;

    if (frame->pts == AV_NOPTS_VALUE || audio_sync_method < 0)
        frame->pts = ost->sync_opts;
    ost->sync_opts = frame->pts + frame->nb_samples;
    ost->samples_encoded += frame->nb_samples;
    ost->frames_encoded++;

    av_assert0(pkt.size || !pkt.data);
    update_benchmark(NULL);
    if (debug_ts) {
        av_log(NULL, AV_LOG_INFO, "encoder <- type:audio "
               "frame_pts:%s frame_pts_time:%s time_base:%d/%d\n",
               av_ts2str(frame->pts), av_ts2timestr(frame->pts, &enc->time_base),
               enc->time_base.num, enc->time_base.den);
    }

    ret = avcodec_send_frame(enc, frame);
    if (ret < 0)
        goto error;

    while (1) {
        ret = avcodec_receive_packet(enc, &pkt);
        if (ret == AVERROR(EAGAIN))
            break;
        if (ret < 0)
            goto error;

        update_benchmark("encode_audio %d.%d", ost->file_index, ost->index);

        av_packet_rescale_ts(&pkt, enc->time_base, ost->mux_timebase);

        if (debug_ts) {
            av_log(NULL, AV_LOG_INFO, "encoder -> type:audio "
                   "pkt_pts:%s pkt_pts_time:%s pkt_dts:%s pkt_dts_time:%s\n",
                   av_ts2str(pkt.pts), av_ts2timestr(pkt.pts, &enc->time_base),
                   av_ts2str(pkt.dts), av_ts2timestr(pkt.dts, &enc->time_base));
        }

        output_packet(of, &pkt, ost, 0);
    }

    return;
error:
    av_log(NULL, AV_LOG_FATAL, "Audio encoding failed\n");
    exit_program(1);
}

static void do_subtitle_out(OutputFile *of,
                            OutputStream *ost,
                            AVSubtitle *sub)
{
    int subtitle_out_max_size = 1024 * 1024;
    int subtitle_out_size, nb, i;
    AVCodecContext *enc;
    AVPacket pkt;
    int64_t pts;

    if (sub->pts == AV_NOPTS_VALUE) {
        av_log(NULL, AV_LOG_ERROR, "Subtitle packets must have a pts\n");
        if (exit_on_error)
            exit_program(1);
        return;
    }

    enc = ost->enc_ctx;

    if (!subtitle_out) {
        subtitle_out = av_malloc(subtitle_out_max_size);
        if (!subtitle_out) {
            av_log(NULL, AV_LOG_FATAL, "Failed to allocate subtitle_out\n");
            exit_program(1);
        }
    }

    /* Note: DVB subtitle need one packet to draw them and one other
       packet to clear them */
    /* XXX: signal it in the codec context ? */
    if (enc->codec_id == AV_CODEC_ID_DVB_SUBTITLE)
        nb = 2;
    else
        nb = 1;

    /* shift timestamp to honor -ss and make check_recording_time() work with -t */
    pts = sub->pts;
    if (output_files[ost->file_index]->start_time != AV_NOPTS_VALUE)
        pts -= output_files[ost->file_index]->start_time;
    for (i = 0; i < nb; i++) {
        unsigned save_num_rects = sub->num_rects;

        ost->sync_opts = av_rescale_q(pts, AV_TIME_BASE_Q, enc->time_base);
        if (!check_recording_time(ost))
            return;

        sub->pts = pts;
        // start_display_time is required to be 0
        sub->pts               += av_rescale_q(sub->start_display_time, (AVRational){ 1, 1000 }, AV_TIME_BASE_Q);
        sub->end_display_time  -= sub->start_display_time;
        sub->start_display_time = 0;
        if (i == 1)
            sub->num_rects = 0;

        ost->frames_encoded++;

        subtitle_out_size = avcodec_encode_subtitle(enc, subtitle_out,
                                                    subtitle_out_max_size, sub);
        if (i == 1)
            sub->num_rects = save_num_rects;
        if (subtitle_out_size < 0) {
            av_log(NULL, AV_LOG_FATAL, "Subtitle encoding failed\n");
            exit_program(1);
        }

        av_init_packet(&pkt);
        pkt.data = subtitle_out;
        pkt.size = subtitle_out_size;
        pkt.pts  = av_rescale_q(sub->pts, AV_TIME_BASE_Q, ost->mux_timebase);
        pkt.duration = av_rescale_q(sub->end_display_time, (AVRational){ 1, 1000 }, ost->mux_timebase);
        if (enc->codec_id == AV_CODEC_ID_DVB_SUBTITLE) {
            /* XXX: the pts correction is handled here. Maybe handling
               it in the codec would be better */
            if (i == 0)
                pkt.pts += av_rescale_q(sub->start_display_time, (AVRational){ 1, 1000 }, ost->mux_timebase);
            else
                pkt.pts += av_rescale_q(sub->end_display_time, (AVRational){ 1, 1000 }, ost->mux_timebase);
        }
        pkt.dts = pkt.pts;
        output_packet(of, &pkt, ost, 0);
    }
}

#include <x264.h>

int gop_num = 0;

#define TOTAL_GOP_NUM 1000

float global_target_score_array[TOTAL_GOP_NUM] = {0.0};
float global_crf_array[TOTAL_GOP_NUM] = {0.0};
extern float global_unsharp_array[TOTAL_GOP_NUM];
float global_aq_strength_array[TOTAL_GOP_NUM] = {0.0};
int   global_frames_of_gop_array[TOTAL_GOP_NUM] = {0};
int   global_decode_gop_num = 0;
int   global_stage1_gop_num = 0;
int   global_stage2_gop_num = 0;
extern int   global_gop;
int   total_gop_num = 0;
long long filtered_frame_num = 0;

typedef struct Eagle_Param_Context {
	float target_score_array[TOTAL_GOP_NUM];
	float crf_array[TOTAL_GOP_NUM];
	float aq_strength_array[TOTAL_GOP_NUM];
	int   frames_of_gop_array[TOTAL_GOP_NUM];
	int   decode_gop_num;
	int   stage1_gop_num;
	int   stage2_gop_num;
	int   total_gop_num;
	long long filtered_frame_num;
}EagleParamContext;


typedef struct X264Context {
    AVClass        *class;
    x264_param_t    params;
    x264_t         *enc;
    x264_picture_t  pic;
    uint8_t        *sei;
    int             sei_size;
    char *preset;
    char *tune;
    char *profile;
    char *level;
    int fastfirstpass;
    char *wpredp;
    char *x264opts;
    float crf;
    float crf_max;
    int cqp;
    int aq_mode;
    float aq_strength;
    char *psy_rd;
    int psy;
    int rc_lookahead;
    int weightp;
    int weightb;
    int ssim;
    int intra_refresh;
    int bluray_compat;
    int b_bias;
    int b_pyramid;
    int mixed_refs;
    int dct8x8;
    int fast_pskip;
    int aud;
    int mbtree;
    char *deblock;
    float cplxblur;
    char *partitions;
    int direct_pred;
    int slice_max_size;
    char *stats;
    int nal_hrd;
    int avcintra_class;
    int motion_est;
    int forced_idr;
    int coder;
    int a53_cc;
    int b_frame_strategy;
    int chroma_offset;
    int scenechange_threshold;
    int noise_reduction;

    char *x264_params;

    int nb_reordered_opaque, next_reordered_opaque;
    int64_t *reordered_opaque;

    /**
     * If the encoder does not support ROI then warn the first time we
     * encounter a frame with ROI side data.
     */
    int roi_warned;
} X264Context;

static void do_video_out(OutputFile *of,
                         OutputStream *ost,
                         AVFrame *next_picture,
                         double sync_ipts)
{
    int ret, format_video_sync;
    AVPacket pkt;
    AVCodecContext *enc = ost->enc_ctx;
    AVCodecParameters *mux_par = ost->st->codecpar;
    AVRational frame_rate;
    int nb_frames, nb0_frames, i;
    double delta, delta0;
    double duration = 0;
    int frame_size = 0;
    InputStream *ist = NULL;
    AVFilterContext *filter = ost->filter->filter;

    if (ost->source_index >= 0)
        ist = input_streams[ost->source_index];

    frame_rate = av_buffersink_get_frame_rate(filter);
    if (frame_rate.num > 0 && frame_rate.den > 0)
        duration = 1/(av_q2d(frame_rate) * av_q2d(enc->time_base));

    if(ist && ist->st->start_time != AV_NOPTS_VALUE && ist->st->first_dts != AV_NOPTS_VALUE && ost->frame_rate.num)
        duration = FFMIN(duration, 1/(av_q2d(ost->frame_rate) * av_q2d(enc->time_base)));

    if (!ost->filters_script &&
        !ost->filters &&
        (nb_filtergraphs == 0 || !filtergraphs[0]->graph_desc) &&
        next_picture &&
        ist &&
        lrintf(next_picture->pkt_duration * av_q2d(ist->st->time_base) / av_q2d(enc->time_base)) > 0) {
        duration = lrintf(next_picture->pkt_duration * av_q2d(ist->st->time_base) / av_q2d(enc->time_base));
    }

    if (!next_picture) {
        //end, flushing
        nb0_frames = nb_frames = mid_pred(ost->last_nb0_frames[0],
                                          ost->last_nb0_frames[1],
                                          ost->last_nb0_frames[2]);
    } else {
        delta0 = sync_ipts - ost->sync_opts; // delta0 is the "drift" between the input frame (next_picture) and where it would fall in the output.
        delta  = delta0 + duration;

        /* by default, we output a single frame */
        nb0_frames = 0; // tracks the number of times the PREVIOUS frame should be duplicated, mostly for variable framerate (VFR)
        nb_frames = 1;

        format_video_sync = video_sync_method;
        if (format_video_sync == VSYNC_AUTO) {
            if(!strcmp(of->ctx->oformat->name, "avi")) {
                format_video_sync = VSYNC_VFR;
            } else
                format_video_sync = (of->ctx->oformat->flags & AVFMT_VARIABLE_FPS) ? ((of->ctx->oformat->flags & AVFMT_NOTIMESTAMPS) ? VSYNC_PASSTHROUGH : VSYNC_VFR) : VSYNC_CFR;
            if (   ist
                && format_video_sync == VSYNC_CFR
                && input_files[ist->file_index]->ctx->nb_streams == 1
                && input_files[ist->file_index]->input_ts_offset == 0) {
                format_video_sync = VSYNC_VSCFR;
            }
            if (format_video_sync == VSYNC_CFR && copy_ts) {
                format_video_sync = VSYNC_VSCFR;
            }
        }
        ost->is_cfr = (format_video_sync == VSYNC_CFR || format_video_sync == VSYNC_VSCFR);

        if (delta0 < 0 &&
            delta > 0 &&
            format_video_sync != VSYNC_PASSTHROUGH &&
            format_video_sync != VSYNC_DROP) {
            if (delta0 < -0.6) {
                av_log(NULL, AV_LOG_VERBOSE, "Past duration %f too large\n", -delta0);
            } else
                av_log(NULL, AV_LOG_DEBUG, "Clipping frame in rate conversion by %f\n", -delta0);
            sync_ipts = ost->sync_opts;
            duration += delta0;
            delta0 = 0;
        }

        switch (format_video_sync) {
        case VSYNC_VSCFR:
            if (ost->frame_number == 0 && delta0 >= 0.5) {
                av_log(NULL, AV_LOG_DEBUG, "Not duplicating %d initial frames\n", (int)lrintf(delta0));
                delta = duration;
                delta0 = 0;
                ost->sync_opts = lrint(sync_ipts);
            }
        case VSYNC_CFR:
            // FIXME set to 0.5 after we fix some dts/pts bugs like in avidec.c
            if (frame_drop_threshold && delta < frame_drop_threshold && ost->frame_number) {
                nb_frames = 0;
            } else if (delta < -1.1)
                nb_frames = 0;
            else if (delta > 1.1) {
                nb_frames = lrintf(delta);
                if (delta0 > 1.1)
                    nb0_frames = lrintf(delta0 - 0.6);
            }
            break;
        case VSYNC_VFR:
            if (delta <= -0.6)
                nb_frames = 0;
            else if (delta > 0.6)
                ost->sync_opts = lrint(sync_ipts);
            break;
        case VSYNC_DROP:
        case VSYNC_PASSTHROUGH:
            ost->sync_opts = lrint(sync_ipts);
            break;
        default:
            av_assert0(0);
        }
    }

    nb_frames = FFMIN(nb_frames, ost->max_frames - ost->frame_number);
    nb0_frames = FFMIN(nb0_frames, nb_frames);

    memmove(ost->last_nb0_frames + 1,
            ost->last_nb0_frames,
            sizeof(ost->last_nb0_frames[0]) * (FF_ARRAY_ELEMS(ost->last_nb0_frames) - 1));
    ost->last_nb0_frames[0] = nb0_frames;

    if (nb0_frames == 0 && ost->last_dropped) {
        nb_frames_drop++;
        av_log(NULL, AV_LOG_VERBOSE,
               "*** dropping frame %d from stream %d at ts %"PRId64"\n",
               ost->frame_number, ost->st->index, ost->last_frame->pts);
    }
    if (nb_frames > (nb0_frames && ost->last_dropped) + (nb_frames > nb0_frames)) {
        if (nb_frames > dts_error_threshold * 30) {
            av_log(NULL, AV_LOG_ERROR, "%d frame duplication too large, skipping\n", nb_frames - 1);
            nb_frames_drop++;
            return;
        }
        nb_frames_dup += nb_frames - (nb0_frames && ost->last_dropped) - (nb_frames > nb0_frames);
        av_log(NULL, AV_LOG_VERBOSE, "*** %d dup!\n", nb_frames - 1);
        if (nb_frames_dup > dup_warning) {
            av_log(NULL, AV_LOG_WARNING, "More than %d frames duplicated\n", dup_warning);
            dup_warning *= 10;
        }
    }
    ost->last_dropped = nb_frames == nb0_frames && next_picture;

    /* duplicates frame if needed */
    for (i = 0; i < nb_frames; i++) {
        AVFrame *in_picture;
        int forced_keyframe = 0;
        double pts_time;
        av_init_packet(&pkt);
        pkt.data = NULL;
        pkt.size = 0;

        if (i < nb0_frames && ost->last_frame) {
            in_picture = ost->last_frame;
        } else
            in_picture = next_picture;

        if (!in_picture)
            return;

        in_picture->pts = ost->sync_opts;

        if (!check_recording_time(ost))
            return;

        if (enc->flags & (AV_CODEC_FLAG_INTERLACED_DCT | AV_CODEC_FLAG_INTERLACED_ME) &&
            ost->top_field_first >= 0)
            in_picture->top_field_first = !!ost->top_field_first;

        if (in_picture->interlaced_frame) {
            if (enc->codec->id == AV_CODEC_ID_MJPEG)
                mux_par->field_order = in_picture->top_field_first ? AV_FIELD_TT:AV_FIELD_BB;
            else
                mux_par->field_order = in_picture->top_field_first ? AV_FIELD_TB:AV_FIELD_BT;
        } else
            mux_par->field_order = AV_FIELD_PROGRESSIVE;

        in_picture->quality = enc->global_quality;
        in_picture->pict_type = 0;

        if (ost->forced_kf_ref_pts == AV_NOPTS_VALUE &&
            in_picture->pts != AV_NOPTS_VALUE)
            ost->forced_kf_ref_pts = in_picture->pts;

        pts_time = in_picture->pts != AV_NOPTS_VALUE ?
            (in_picture->pts - ost->forced_kf_ref_pts) * av_q2d(enc->time_base) : NAN;
        if (ost->forced_kf_index < ost->forced_kf_count &&
            in_picture->pts >= ost->forced_kf_pts[ost->forced_kf_index]) {
            ost->forced_kf_index++;
            forced_keyframe = 1;
        } else if (ost->forced_keyframes_pexpr) {
            double res;
            ost->forced_keyframes_expr_const_values[FKF_T] = pts_time;
            res = av_expr_eval(ost->forced_keyframes_pexpr,
                               ost->forced_keyframes_expr_const_values, NULL);
            ff_dlog(NULL, "force_key_frame: n:%f n_forced:%f prev_forced_n:%f t:%f prev_forced_t:%f -> res:%f\n",
                    ost->forced_keyframes_expr_const_values[FKF_N],
                    ost->forced_keyframes_expr_const_values[FKF_N_FORCED],
                    ost->forced_keyframes_expr_const_values[FKF_PREV_FORCED_N],
                    ost->forced_keyframes_expr_const_values[FKF_T],
                    ost->forced_keyframes_expr_const_values[FKF_PREV_FORCED_T],
                    res);
            if (res) {
                forced_keyframe = 1;
                ost->forced_keyframes_expr_const_values[FKF_PREV_FORCED_N] =
                    ost->forced_keyframes_expr_const_values[FKF_N];
                ost->forced_keyframes_expr_const_values[FKF_PREV_FORCED_T] =
                    ost->forced_keyframes_expr_const_values[FKF_T];
                ost->forced_keyframes_expr_const_values[FKF_N_FORCED] += 1;
            }

            ost->forced_keyframes_expr_const_values[FKF_N] += 1;
        } else if (   ost->forced_keyframes
                   && !strncmp(ost->forced_keyframes, "source", 6)
                   && in_picture->key_frame==1) {
            forced_keyframe = 1;
        }

        if (forced_keyframe) {
            in_picture->pict_type = AV_PICTURE_TYPE_I;
            av_log(NULL, AV_LOG_DEBUG, "Forced keyframe at time %f\n", pts_time);
        }

        update_benchmark(NULL);
        if (debug_ts) {
            av_log(NULL, AV_LOG_INFO, "encoder <- type:video "
                   "frame_pts:%s frame_pts_time:%s time_base:%d/%d\n",
                   av_ts2str(in_picture->pts), av_ts2timestr(in_picture->pts, &enc->time_base),
                   enc->time_base.num, enc->time_base.den);
        }

        ost->frames_encoded++;

        //TODO:eagle should add
        //reconfig encoder params(aq_strength, crf), using the x264_encoder_reconfig
        //example: X264Context *x4 = enc->priv_data;
        //x4->params.rc_f_rf_constant = x4->crf = crx_value;
        //x4->params.rc_f_aq_strength = x4->aq_strength = aq_strength_value;
        {
        	long long total_encoded_frame_num = 0;
			X264Context *x4 = enc->priv_data;

			global_frames_of_gop_array[global_gop];
			for (int i = 0; i <= global_gop; i++) {
				total_encoded_frame_num += global_frames_of_gop_array[i];
			}

			if (ost->frames_encoded > total_encoded_frame_num && global_gop < total_gop_num)
				global_gop++;

			x4->params.rc.f_rf_constant = x4->crf         = global_crf_array[global_gop];
			x4->params.rc.f_aq_strength = x4->aq_strength = global_aq_strength_array[global_gop];

			x264_encoder_reconfig(x4->enc, &x4->params);
	        printf("ost->frames_encoded %d global_gop %d crf %f aq_strength_definite %f x4->aq_strength %f x4->params.rc.f_aq_strength %f\n",
				ost->frames_encoded, global_gop, global_crf_array[global_gop], global_aq_strength_array[global_gop],
				x4->aq_strength, x4->params.rc.f_aq_strength);	
        }

        ret = avcodec_send_frame(enc, in_picture);
        if (ret < 0)
            goto error;
        // Make sure Closed Captions will not be duplicated
        av_frame_remove_side_data(in_picture, AV_FRAME_DATA_A53_CC);

        while (1) {
            ret = avcodec_receive_packet(enc, &pkt);
            update_benchmark("encode_video %d.%d", ost->file_index, ost->index);
            if (ret == AVERROR(EAGAIN))
                break;
            if (ret < 0)
                goto error;

            if (debug_ts) {
                av_log(NULL, AV_LOG_INFO, "encoder -> type:video "
                       "pkt_pts:%s pkt_pts_time:%s pkt_dts:%s pkt_dts_time:%s\n",
                       av_ts2str(pkt.pts), av_ts2timestr(pkt.pts, &enc->time_base),
                       av_ts2str(pkt.dts), av_ts2timestr(pkt.dts, &enc->time_base));
            }

            if (pkt.pts == AV_NOPTS_VALUE && !(enc->codec->capabilities & AV_CODEC_CAP_DELAY))
                pkt.pts = ost->sync_opts;

            av_packet_rescale_ts(&pkt, enc->time_base, ost->mux_timebase);

            if (debug_ts) {
                av_log(NULL, AV_LOG_INFO, "encoder -> type:video "
                    "pkt_pts:%s pkt_pts_time:%s pkt_dts:%s pkt_dts_time:%s\n",
                    av_ts2str(pkt.pts), av_ts2timestr(pkt.pts, &ost->mux_timebase),
                    av_ts2str(pkt.dts), av_ts2timestr(pkt.dts, &ost->mux_timebase));
            }

            frame_size = pkt.size;
            output_packet(of, &pkt, ost, 0);

            /* if two pass, output log */
            if (ost->logfile && enc->stats_out) {
                fprintf(ost->logfile, "%s", enc->stats_out);
            }
        }
        ost->sync_opts++;
        /*
         * For video, number of frames in == number of packets out.
         * But there may be reordering, so we can't throw away frames on encoder
         * flush, we need to limit them here, before they go into encoder.
         */
        ost->frame_number++;

        if (vstats_filename && frame_size)
            do_video_stats(ost, frame_size);
    }

    if (!ost->last_frame)
        ost->last_frame = av_frame_alloc();
    av_frame_unref(ost->last_frame);
    if (next_picture && ost->last_frame)
        av_frame_ref(ost->last_frame, next_picture);
    else
        av_frame_free(&ost->last_frame);

    return;
error:
    av_log(NULL, AV_LOG_FATAL, "Video encoding failed\n");
    exit_program(1);
}

static double psnr(double d)
{
    return -10.0 * log10(d);
}

static void do_video_stats(OutputStream *ost, int frame_size)
{
    AVCodecContext *enc;
    int frame_number;
    double ti1, bitrate, avg_bitrate;

    /* this is executed just the first time do_video_stats is called */
    if (!vstats_file) {
        vstats_file = fopen(vstats_filename, "w");
        if (!vstats_file) {
            perror("fopen");
            exit_program(1);
        }
    }

    enc = ost->enc_ctx;
    if (enc->codec_type == AVMEDIA_TYPE_VIDEO) {
        frame_number = ost->st->nb_frames;
        if (vstats_version <= 1) {
            fprintf(vstats_file, "frame= %5d q= %2.1f ", frame_number,
                    ost->quality / (float)FF_QP2LAMBDA);
        } else  {
            fprintf(vstats_file, "out= %2d st= %2d frame= %5d q= %2.1f ", ost->file_index, ost->index, frame_number,
                    ost->quality / (float)FF_QP2LAMBDA);
        }

        if (ost->error[0]>=0 && (enc->flags & AV_CODEC_FLAG_PSNR))
            fprintf(vstats_file, "PSNR= %6.2f ", psnr(ost->error[0] / (enc->width * enc->height * 255.0 * 255.0)));

        fprintf(vstats_file,"f_size= %6d ", frame_size);
        /* compute pts value */
        ti1 = av_stream_get_end_pts(ost->st) * av_q2d(ost->st->time_base);
        if (ti1 < 0.01)
            ti1 = 0.01;

        bitrate     = (frame_size * 8) / av_q2d(enc->time_base) / 1000.0;
        avg_bitrate = (double)(ost->data_size * 8) / ti1 / 1000.0;
        fprintf(vstats_file, "s_size= %8.0fkB time= %0.3f br= %7.1fkbits/s avg_br= %7.1fkbits/s ",
               (double)ost->data_size / 1024, ti1, bitrate, avg_bitrate);
        fprintf(vstats_file, "type= %c\n", av_get_picture_type_char(ost->pict_type));
    }
}

static int init_output_stream(OutputStream *ost, char *error, int error_len);

static void finish_output_stream(OutputStream *ost)
{
    OutputFile *of = output_files[ost->file_index];
    int i;

    ost->finished = ENCODER_FINISHED | MUXER_FINISHED;

    if (of->shortest) {
        for (i = 0; i < of->ctx->nb_streams; i++)
            output_streams[of->ost_index + i]->finished = ENCODER_FINISHED | MUXER_FINISHED;
    }
}

/**
 * Get and encode new output from any of the filtergraphs, without causing
 * activity.
 *
 * @return  0 for success, <0 for severe errors
 */
static int reap_filters(int flush)
{
    AVFrame *filtered_frame = NULL;
    int i;

    /* Reap all buffers present in the buffer sinks */
    for (i = 0; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];
        OutputFile    *of = output_files[ost->file_index];
        AVFilterContext *filter;
        AVCodecContext *enc = ost->enc_ctx;
        int ret = 0;

        if (!ost->filter || !ost->filter->graph->graph)
            continue;
        filter = ost->filter->filter;

        if (!ost->initialized) {
            char error[1024] = "";
            ret = init_output_stream(ost, error, sizeof(error));
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error initializing output stream %d:%d -- %s\n",
                       ost->file_index, ost->index, error);
                exit_program(1);
            }
        }

        if (!ost->filtered_frame && !(ost->filtered_frame = av_frame_alloc())) {
            return AVERROR(ENOMEM);
        }
        filtered_frame = ost->filtered_frame;

        while (1) {
            double float_pts = AV_NOPTS_VALUE; // this is identical to filtered_frame.pts but with higher precision
            ret = av_buffersink_get_frame_flags(filter, filtered_frame,
                                               AV_BUFFERSINK_FLAG_NO_REQUEST);
            if (ret < 0) {
                if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                    av_log(NULL, AV_LOG_WARNING,
                           "Error in av_buffersink_get_frame_flags(): %s\n", av_err2str(ret));
                } else if (flush && ret == AVERROR_EOF) {
                    if (av_buffersink_get_type(filter) == AVMEDIA_TYPE_VIDEO)
                        do_video_out(of, ost, NULL, AV_NOPTS_VALUE);
                }
                break;
            }
            if (ost->finished) {
                av_frame_unref(filtered_frame);
                continue;
            }
            if (filtered_frame->pts != AV_NOPTS_VALUE) {
                int64_t start_time = (of->start_time == AV_NOPTS_VALUE) ? 0 : of->start_time;
                AVRational filter_tb = av_buffersink_get_time_base(filter);
                AVRational tb = enc->time_base;
                int extra_bits = av_clip(29 - av_log2(tb.den), 0, 16);

                tb.den <<= extra_bits;
                float_pts =
                    av_rescale_q(filtered_frame->pts, filter_tb, tb) -
                    av_rescale_q(start_time, AV_TIME_BASE_Q, tb);
                float_pts /= 1 << extra_bits;
                // avoid exact midoints to reduce the chance of rounding differences, this can be removed in case the fps code is changed to work with integers
                float_pts += FFSIGN(float_pts) * 1.0 / (1<<17);

                filtered_frame->pts =
                    av_rescale_q(filtered_frame->pts, filter_tb, enc->time_base) -
                    av_rescale_q(start_time, AV_TIME_BASE_Q, enc->time_base);
            }

            switch (av_buffersink_get_type(filter)) {
            case AVMEDIA_TYPE_VIDEO:
                if (!ost->frame_aspect_ratio.num)
                    enc->sample_aspect_ratio = filtered_frame->sample_aspect_ratio;

                if (debug_ts) {
                    av_log(NULL, AV_LOG_INFO, "filter -> pts:%s pts_time:%s exact:%f time_base:%d/%d\n",
                            av_ts2str(filtered_frame->pts), av_ts2timestr(filtered_frame->pts, &enc->time_base),
                            float_pts,
                            enc->time_base.num, enc->time_base.den);
                }

                do_video_out(of, ost, filtered_frame, float_pts);
                break;
            case AVMEDIA_TYPE_AUDIO:
                if (!(enc->codec->capabilities & AV_CODEC_CAP_PARAM_CHANGE) &&
                    enc->channels != filtered_frame->channels) {
                    av_log(NULL, AV_LOG_ERROR,
                           "Audio filter graph output is not normalized and encoder does not support parameter changes\n");
                    break;
                }
                do_audio_out(of, ost, filtered_frame);
                break;
            default:
                // TODO support subtitle filters
                av_assert0(0);
            }

            av_frame_unref(filtered_frame);
        }
    }

    return 0;
}

static void print_final_stats(int64_t total_size)
{
    uint64_t video_size = 0, audio_size = 0, extra_size = 0, other_size = 0;
    uint64_t subtitle_size = 0;
    uint64_t data_size = 0;
    float percent = -1.0;
    int i, j;
    int pass1_used = 1;

    for (i = 0; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];
        switch (ost->enc_ctx->codec_type) {
            case AVMEDIA_TYPE_VIDEO: video_size += ost->data_size; break;
            case AVMEDIA_TYPE_AUDIO: audio_size += ost->data_size; break;
            case AVMEDIA_TYPE_SUBTITLE: subtitle_size += ost->data_size; break;
            default:                 other_size += ost->data_size; break;
        }
        extra_size += ost->enc_ctx->extradata_size;
        data_size  += ost->data_size;
        if (   (ost->enc_ctx->flags & (AV_CODEC_FLAG_PASS1 | AV_CODEC_FLAG_PASS2))
            != AV_CODEC_FLAG_PASS1)
            pass1_used = 0;
    }

    if (data_size && total_size>0 && total_size >= data_size)
        percent = 100.0 * (total_size - data_size) / data_size;

    av_log(NULL, AV_LOG_INFO, "video:%1.0fkB audio:%1.0fkB subtitle:%1.0fkB other streams:%1.0fkB global headers:%1.0fkB muxing overhead: ",
           video_size / 1024.0,
           audio_size / 1024.0,
           subtitle_size / 1024.0,
           other_size / 1024.0,
           extra_size / 1024.0);
    if (percent >= 0.0)
        av_log(NULL, AV_LOG_INFO, "%f%%", percent);
    else
        av_log(NULL, AV_LOG_INFO, "unknown");
    av_log(NULL, AV_LOG_INFO, "\n");

    /* print verbose per-stream stats */
    for (i = 0; i < nb_input_files; i++) {
        InputFile *f = input_files[i];
        uint64_t total_packets = 0, total_size = 0;

        av_log(NULL, AV_LOG_VERBOSE, "Input file #%d (%s):\n",
               i, f->ctx->url);

        for (j = 0; j < f->nb_streams; j++) {
            InputStream *ist = input_streams[f->ist_index + j];
            enum AVMediaType type = ist->dec_ctx->codec_type;

            total_size    += ist->data_size;
            total_packets += ist->nb_packets;

            av_log(NULL, AV_LOG_VERBOSE, "  Input stream #%d:%d (%s): ",
                   i, j, media_type_string(type));
            av_log(NULL, AV_LOG_VERBOSE, "%"PRIu64" packets read (%"PRIu64" bytes); ",
                   ist->nb_packets, ist->data_size);

            if (ist->decoding_needed) {
                av_log(NULL, AV_LOG_VERBOSE, "%"PRIu64" frames decoded",
                       ist->frames_decoded);
                if (type == AVMEDIA_TYPE_AUDIO)
                    av_log(NULL, AV_LOG_VERBOSE, " (%"PRIu64" samples)", ist->samples_decoded);
                av_log(NULL, AV_LOG_VERBOSE, "; ");
            }

            av_log(NULL, AV_LOG_VERBOSE, "\n");
        }

        av_log(NULL, AV_LOG_VERBOSE, "  Total: %"PRIu64" packets (%"PRIu64" bytes) demuxed\n",
               total_packets, total_size);
    }

    for (i = 0; i < nb_output_files; i++) {
        OutputFile *of = output_files[i];
        uint64_t total_packets = 0, total_size = 0;

        av_log(NULL, AV_LOG_VERBOSE, "Output file #%d (%s):\n",
               i, of->ctx->url);

        for (j = 0; j < of->ctx->nb_streams; j++) {
            OutputStream *ost = output_streams[of->ost_index + j];
            enum AVMediaType type = ost->enc_ctx->codec_type;

            total_size    += ost->data_size;
            total_packets += ost->packets_written;

            av_log(NULL, AV_LOG_VERBOSE, "  Output stream #%d:%d (%s): ",
                   i, j, media_type_string(type));
            if (ost->encoding_needed) {
                av_log(NULL, AV_LOG_VERBOSE, "%"PRIu64" frames encoded",
                       ost->frames_encoded);
                if (type == AVMEDIA_TYPE_AUDIO)
                    av_log(NULL, AV_LOG_VERBOSE, " (%"PRIu64" samples)", ost->samples_encoded);
                av_log(NULL, AV_LOG_VERBOSE, "; ");
            }

            av_log(NULL, AV_LOG_VERBOSE, "%"PRIu64" packets muxed (%"PRIu64" bytes); ",
                   ost->packets_written, ost->data_size);

            av_log(NULL, AV_LOG_VERBOSE, "\n");
        }

        av_log(NULL, AV_LOG_VERBOSE, "  Total: %"PRIu64" packets (%"PRIu64" bytes) muxed\n",
               total_packets, total_size);
    }
    if(video_size + data_size + audio_size + subtitle_size + extra_size == 0){
        av_log(NULL, AV_LOG_WARNING, "Output file is empty, nothing was encoded ");
        if (pass1_used) {
            av_log(NULL, AV_LOG_WARNING, "\n");
        } else {
            av_log(NULL, AV_LOG_WARNING, "(check -ss / -t / -frames parameters if used)\n");
        }
    }
}

static void print_report(int is_last_report, int64_t timer_start, int64_t cur_time)
{
    AVBPrint buf, buf_script;
    OutputStream *ost;
    AVFormatContext *oc;
    int64_t total_size;
    AVCodecContext *enc;
    int frame_number, vid, i;
    double bitrate;
    double speed;
    int64_t pts = INT64_MIN + 1;
    static int64_t last_time = -1;
    static int qp_histogram[52];
    int hours, mins, secs, us;
    const char *hours_sign;
    int ret;
    float t;

    if (!print_stats && !is_last_report && !progress_avio)
        return;

    if (!is_last_report) {
        if (last_time == -1) {
            last_time = cur_time;
            return;
        }
        if ((cur_time - last_time) < 500000)
            return;
        last_time = cur_time;
    }

    t = (cur_time-timer_start) / 1000000.0;


    oc = output_files[0]->ctx;

    total_size = avio_size(oc->pb);
    if (total_size <= 0) // FIXME improve avio_size() so it works with non seekable output too
        total_size = avio_tell(oc->pb);

    vid = 0;
    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
    av_bprint_init(&buf_script, 0, AV_BPRINT_SIZE_AUTOMATIC);
    for (i = 0; i < nb_output_streams; i++) {
        float q = -1;
        ost = output_streams[i];
        enc = ost->enc_ctx;
        if (!ost->stream_copy)
            q = ost->quality / (float) FF_QP2LAMBDA;

        if (vid && enc->codec_type == AVMEDIA_TYPE_VIDEO) {
            av_bprintf(&buf, "q=%2.1f ", q);
            av_bprintf(&buf_script, "stream_%d_%d_q=%.1f\n",
                       ost->file_index, ost->index, q);
        }
        if (!vid && enc->codec_type == AVMEDIA_TYPE_VIDEO) {
            float fps;

            frame_number = ost->frame_number;
            fps = t > 1 ? frame_number / t : 0;
            av_bprintf(&buf, "frame=%5d fps=%3.*f q=%3.1f ",
                     frame_number, fps < 9.95, fps, q);
            av_bprintf(&buf_script, "frame=%d\n", frame_number);
            av_bprintf(&buf_script, "fps=%.2f\n", fps);
            av_bprintf(&buf_script, "stream_%d_%d_q=%.1f\n",
                       ost->file_index, ost->index, q);
            if (is_last_report)
                av_bprintf(&buf, "L");
            if (qp_hist) {
                int j;
                int qp = lrintf(q);
                if (qp >= 0 && qp < FF_ARRAY_ELEMS(qp_histogram))
                    qp_histogram[qp]++;
                for (j = 0; j < 32; j++)
                    av_bprintf(&buf, "%X", av_log2(qp_histogram[j] + 1));
            }

            if ((enc->flags & AV_CODEC_FLAG_PSNR) && (ost->pict_type != AV_PICTURE_TYPE_NONE || is_last_report)) {
                int j;
                double error, error_sum = 0;
                double scale, scale_sum = 0;
                double p;
                char type[3] = { 'Y','U','V' };
                av_bprintf(&buf, "PSNR=");
                for (j = 0; j < 3; j++) {
                    if (is_last_report) {
                        error = enc->error[j];
                        scale = enc->width * enc->height * 255.0 * 255.0 * frame_number;
                    } else {
                        error = ost->error[j];
                        scale = enc->width * enc->height * 255.0 * 255.0;
                    }
                    if (j)
                        scale /= 4;
                    error_sum += error;
                    scale_sum += scale;
                    p = psnr(error / scale);
                    av_bprintf(&buf, "%c:%2.2f ", type[j], p);
                    av_bprintf(&buf_script, "stream_%d_%d_psnr_%c=%2.2f\n",
                               ost->file_index, ost->index, type[j] | 32, p);
                }
                p = psnr(error_sum / scale_sum);
                av_bprintf(&buf, "*:%2.2f ", psnr(error_sum / scale_sum));
                av_bprintf(&buf_script, "stream_%d_%d_psnr_all=%2.2f\n",
                           ost->file_index, ost->index, p);
            }
            vid = 1;
        }
        /* compute min output value */
        if (av_stream_get_end_pts(ost->st) != AV_NOPTS_VALUE)
            pts = FFMAX(pts, av_rescale_q(av_stream_get_end_pts(ost->st),
                                          ost->st->time_base, AV_TIME_BASE_Q));
        if (is_last_report)
            nb_frames_drop += ost->last_dropped;
    }

    secs = FFABS(pts) / AV_TIME_BASE;
    us = FFABS(pts) % AV_TIME_BASE;
    mins = secs / 60;
    secs %= 60;
    hours = mins / 60;
    mins %= 60;
    hours_sign = (pts < 0) ? "-" : "";

    bitrate = pts && total_size >= 0 ? total_size * 8 / (pts / 1000.0) : -1;
    speed = t != 0.0 ? (double)pts / AV_TIME_BASE / t : -1;

    if (total_size < 0) av_bprintf(&buf, "size=N/A time=");
    else                av_bprintf(&buf, "size=%8.0fkB time=", total_size / 1024.0);
    if (pts == AV_NOPTS_VALUE) {
        av_bprintf(&buf, "N/A ");
    } else {
        av_bprintf(&buf, "%s%02d:%02d:%02d.%02d ",
                   hours_sign, hours, mins, secs, (100 * us) / AV_TIME_BASE);
    }

    if (bitrate < 0) {
        av_bprintf(&buf, "bitrate=N/A");
        av_bprintf(&buf_script, "bitrate=N/A\n");
    }else{
        av_bprintf(&buf, "bitrate=%6.1fkbits/s", bitrate);
        av_bprintf(&buf_script, "bitrate=%6.1fkbits/s\n", bitrate);
    }

    if (total_size < 0) av_bprintf(&buf_script, "total_size=N/A\n");
    else                av_bprintf(&buf_script, "total_size=%"PRId64"\n", total_size);
    if (pts == AV_NOPTS_VALUE) {
        av_bprintf(&buf_script, "out_time_us=N/A\n");
        av_bprintf(&buf_script, "out_time_ms=N/A\n");
        av_bprintf(&buf_script, "out_time=N/A\n");
    } else {
        av_bprintf(&buf_script, "out_time_us=%"PRId64"\n", pts);
        av_bprintf(&buf_script, "out_time_ms=%"PRId64"\n", pts);
        av_bprintf(&buf_script, "out_time=%s%02d:%02d:%02d.%06d\n",
                   hours_sign, hours, mins, secs, us);
    }

    if (nb_frames_dup || nb_frames_drop)
        av_bprintf(&buf, " dup=%d drop=%d", nb_frames_dup, nb_frames_drop);
    av_bprintf(&buf_script, "dup_frames=%d\n", nb_frames_dup);
    av_bprintf(&buf_script, "drop_frames=%d\n", nb_frames_drop);

    if (speed < 0) {
        av_bprintf(&buf, " speed=N/A");
        av_bprintf(&buf_script, "speed=N/A\n");
    } else {
        av_bprintf(&buf, " speed=%4.3gx", speed);
        av_bprintf(&buf_script, "speed=%4.3gx\n", speed);
    }

    if (print_stats || is_last_report) {
        const char end = is_last_report ? '\n' : '\r';
        if (print_stats==1 && AV_LOG_INFO > av_log_get_level()) {
            fprintf(stderr, "%s    %c", buf.str, end);
        } else
            av_log(NULL, AV_LOG_INFO, "%s    %c", buf.str, end);

        fflush(stderr);
    }
    av_bprint_finalize(&buf, NULL);

    if (progress_avio) {
        av_bprintf(&buf_script, "progress=%s\n",
                   is_last_report ? "end" : "continue");
        avio_write(progress_avio, buf_script.str,
                   FFMIN(buf_script.len, buf_script.size - 1));
        avio_flush(progress_avio);
        av_bprint_finalize(&buf_script, NULL);
        if (is_last_report) {
            if ((ret = avio_closep(&progress_avio)) < 0)
                av_log(NULL, AV_LOG_ERROR,
                       "Error closing progress log, loss of information possible: %s\n", av_err2str(ret));
        }
    }

    if (is_last_report)
        print_final_stats(total_size);
}

static void ifilter_parameters_from_codecpar(InputFilter *ifilter, AVCodecParameters *par)
{
    // We never got any input. Set a fake format, which will
    // come from libavformat.
    ifilter->format                 = par->format;
    ifilter->sample_rate            = par->sample_rate;
    ifilter->channels               = par->channels;
    ifilter->channel_layout         = par->channel_layout;
    ifilter->width                  = par->width;
    ifilter->height                 = par->height;
    ifilter->sample_aspect_ratio    = par->sample_aspect_ratio;
}

static void flush_encoders(void)
{
    int i, ret;

    for (i = 0; i < nb_output_streams; i++) {
        OutputStream   *ost = output_streams[i];
        AVCodecContext *enc = ost->enc_ctx;
        OutputFile      *of = output_files[ost->file_index];

        if (!ost->encoding_needed)
            continue;

        // Try to enable encoding with no input frames.
        // Maybe we should just let encoding fail instead.
        if (!ost->initialized) {
            FilterGraph *fg = ost->filter->graph;
            char error[1024] = "";

            av_log(NULL, AV_LOG_WARNING,
                   "Finishing stream %d:%d without any data written to it.\n",
                   ost->file_index, ost->st->index);

            if (ost->filter && !fg->graph) {
                int x;
                for (x = 0; x < fg->nb_inputs; x++) {
                    InputFilter *ifilter = fg->inputs[x];
                    if (ifilter->format < 0)
                        ifilter_parameters_from_codecpar(ifilter, ifilter->ist->st->codecpar);
                }

                if (!ifilter_has_all_input_formats(fg))
                    continue;

                ret = configure_filtergraph(fg);
                if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error configuring filter graph\n");
                    exit_program(1);
                }

                finish_output_stream(ost);
            }

            ret = init_output_stream(ost, error, sizeof(error));
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error initializing output stream %d:%d -- %s\n",
                       ost->file_index, ost->index, error);
                exit_program(1);
            }
        }

        if (enc->codec_type == AVMEDIA_TYPE_AUDIO && enc->frame_size <= 1)
            continue;

        if (enc->codec_type != AVMEDIA_TYPE_VIDEO && enc->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;

        for (;;) {
            const char *desc = NULL;
            AVPacket pkt;
            int pkt_size;

            switch (enc->codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                desc   = "audio";
                break;
            case AVMEDIA_TYPE_VIDEO:
                desc   = "video";
                break;
            default:
                av_assert0(0);
            }

            av_init_packet(&pkt);
            pkt.data = NULL;
            pkt.size = 0;

            update_benchmark(NULL);

            while ((ret = avcodec_receive_packet(enc, &pkt)) == AVERROR(EAGAIN)) {
                ret = avcodec_send_frame(enc, NULL);
                if (ret < 0) {
                    av_log(NULL, AV_LOG_FATAL, "%s encoding failed: %s\n",
                           desc,
                           av_err2str(ret));
                    exit_program(1);
                }
            }

            update_benchmark("flush_%s %d.%d", desc, ost->file_index, ost->index);
            if (ret < 0 && ret != AVERROR_EOF) {
                av_log(NULL, AV_LOG_FATAL, "%s encoding failed: %s\n",
                       desc,
                       av_err2str(ret));
                exit_program(1);
            }
            if (ost->logfile && enc->stats_out) {
                fprintf(ost->logfile, "%s", enc->stats_out);
            }
            if (ret == AVERROR_EOF) {
                output_packet(of, &pkt, ost, 1);
                break;
            }
            if (ost->finished & MUXER_FINISHED) {
                av_packet_unref(&pkt);
                continue;
            }
            av_packet_rescale_ts(&pkt, enc->time_base, ost->mux_timebase);
            pkt_size = pkt.size;
            output_packet(of, &pkt, ost, 0);
            if (ost->enc_ctx->codec_type == AVMEDIA_TYPE_VIDEO && vstats_filename) {
                do_video_stats(ost, pkt_size);
            }
        }
    }
}

/*
 * Check whether a packet from ist should be written into ost at this time
 */
static int check_output_constraints(InputStream *ist, OutputStream *ost)
{
    OutputFile *of = output_files[ost->file_index];
    int ist_index  = input_files[ist->file_index]->ist_index + ist->st->index;

    if (ost->source_index != ist_index)
        return 0;

    if (ost->finished)
        return 0;

    if (of->start_time != AV_NOPTS_VALUE && ist->pts < of->start_time)
        return 0;

    return 1;
}

static void do_streamcopy(InputStream *ist, OutputStream *ost, const AVPacket *pkt)
{
    OutputFile *of = output_files[ost->file_index];
    InputFile   *f = input_files [ist->file_index];
    int64_t start_time = (of->start_time == AV_NOPTS_VALUE) ? 0 : of->start_time;
    int64_t ost_tb_start_time = av_rescale_q(start_time, AV_TIME_BASE_Q, ost->mux_timebase);
    AVPacket opkt;

    // EOF: flush output bitstream filters.
    if (!pkt) {
        av_init_packet(&opkt);
        opkt.data = NULL;
        opkt.size = 0;
        output_packet(of, &opkt, ost, 1);
        return;
    }

    if ((!ost->frame_number && !(pkt->flags & AV_PKT_FLAG_KEY)) &&
        !ost->copy_initial_nonkeyframes)
        return;

    if (!ost->frame_number && !ost->copy_prior_start) {
        int64_t comp_start = start_time;
        if (copy_ts && f->start_time != AV_NOPTS_VALUE)
            comp_start = FFMAX(start_time, f->start_time + f->ts_offset);
        if (pkt->pts == AV_NOPTS_VALUE ?
            ist->pts < comp_start :
            pkt->pts < av_rescale_q(comp_start, AV_TIME_BASE_Q, ist->st->time_base))
            return;
    }

    if (of->recording_time != INT64_MAX &&
        ist->pts >= of->recording_time + start_time) {
        close_output_stream(ost);
        return;
    }

    if (f->recording_time != INT64_MAX) {
        start_time = f->ctx->start_time;
        if (f->start_time != AV_NOPTS_VALUE && copy_ts)
            start_time += f->start_time;
        if (ist->pts >= f->recording_time + start_time) {
            close_output_stream(ost);
            return;
        }
    }

    /* force the input stream PTS */
    if (ost->enc_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
        ost->sync_opts++;

    if (av_packet_ref(&opkt, pkt) < 0)
        exit_program(1);

    if (pkt->pts != AV_NOPTS_VALUE)
        opkt.pts = av_rescale_q(pkt->pts, ist->st->time_base, ost->mux_timebase) - ost_tb_start_time;

    if (pkt->dts == AV_NOPTS_VALUE)
        opkt.dts = av_rescale_q(ist->dts, AV_TIME_BASE_Q, ost->mux_timebase);
    else
        opkt.dts = av_rescale_q(pkt->dts, ist->st->time_base, ost->mux_timebase);
    opkt.dts -= ost_tb_start_time;

    if (ost->st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && pkt->dts != AV_NOPTS_VALUE) {
        int duration = av_get_audio_frame_duration(ist->dec_ctx, pkt->size);
        if(!duration)
            duration = ist->dec_ctx->frame_size;
        opkt.dts = opkt.pts = av_rescale_delta(ist->st->time_base, pkt->dts,
                                               (AVRational){1, ist->dec_ctx->sample_rate}, duration, &ist->filter_in_rescale_delta_last,
                                               ost->mux_timebase) - ost_tb_start_time;
    }

    opkt.duration = av_rescale_q(pkt->duration, ist->st->time_base, ost->mux_timebase);

    output_packet(of, &opkt, ost, 0);
}

int guess_input_channel_layout(InputStream *ist)
{
    AVCodecContext *dec = ist->dec_ctx;

    if (!dec->channel_layout) {
        char layout_name[256];

        if (dec->channels > ist->guess_layout_max)
            return 0;
        dec->channel_layout = av_get_default_channel_layout(dec->channels);
        if (!dec->channel_layout)
            return 0;
        av_get_channel_layout_string(layout_name, sizeof(layout_name),
                                     dec->channels, dec->channel_layout);
        av_log(NULL, AV_LOG_WARNING, "Guessed Channel Layout for Input Stream "
               "#%d.%d : %s\n", ist->file_index, ist->st->index, layout_name);
    }
    return 1;
}

static void check_decode_result(InputStream *ist, int *got_output, int ret)
{
    if (*got_output || ret<0)
        decode_error_stat[ret<0] ++;

    if (ret < 0 && exit_on_error)
        exit_program(1);

    if (*got_output && ist) {
        if (ist->decoded_frame->decode_error_flags || (ist->decoded_frame->flags & AV_FRAME_FLAG_CORRUPT)) {
            av_log(NULL, exit_on_error ? AV_LOG_FATAL : AV_LOG_WARNING,
                   "%s: corrupt decoded frame in stream %d\n", input_files[ist->file_index]->ctx->url, ist->st->index);
            if (exit_on_error)
                exit_program(1);
        }
    }
}

// Filters can be configured only if the formats of all inputs are known.
static int ifilter_has_all_input_formats(FilterGraph *fg)
{
    int i;
    for (i = 0; i < fg->nb_inputs; i++) {
        if (fg->inputs[i]->format < 0 && (fg->inputs[i]->type == AVMEDIA_TYPE_AUDIO ||
                                          fg->inputs[i]->type == AVMEDIA_TYPE_VIDEO))
            return 0;
    }
    return 1;
}

static int ifilter_send_frame(InputFilter *ifilter, AVFrame *frame)
{
    FilterGraph *fg = ifilter->graph;
    int need_reinit, ret, i;

    /* determine if the parameters for this input changed */
    need_reinit = ifilter->format != frame->format;

    switch (ifilter->ist->st->codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        need_reinit |= ifilter->sample_rate    != frame->sample_rate ||
                       ifilter->channels       != frame->channels ||
                       ifilter->channel_layout != frame->channel_layout;
        break;
    case AVMEDIA_TYPE_VIDEO:
        need_reinit |= ifilter->width  != frame->width ||
                       ifilter->height != frame->height;
        break;
    }

    if (!ifilter->ist->reinit_filters && fg->graph)
        need_reinit = 0;

    if (!!ifilter->hw_frames_ctx != !!frame->hw_frames_ctx ||
        (ifilter->hw_frames_ctx && ifilter->hw_frames_ctx->data != frame->hw_frames_ctx->data))
        need_reinit = 1;

    if (need_reinit) {
        ret = ifilter_parameters_from_frame(ifilter, frame);
        if (ret < 0)
            return ret;
    }

    /* (re)init the graph if possible, otherwise buffer the frame and return */
    if (need_reinit || !fg->graph) {
        for (i = 0; i < fg->nb_inputs; i++) {
            if (!ifilter_has_all_input_formats(fg)) {
                AVFrame *tmp = av_frame_clone(frame);
                if (!tmp)
                    return AVERROR(ENOMEM);
                av_frame_unref(frame);

                if (!av_fifo_space(ifilter->frame_queue)) {
                    ret = av_fifo_realloc2(ifilter->frame_queue, 2 * av_fifo_size(ifilter->frame_queue));
                    if (ret < 0) {
                        av_frame_free(&tmp);
                        return ret;
                    }
                }
                av_fifo_generic_write(ifilter->frame_queue, &tmp, sizeof(tmp), NULL);
                return 0;
            }
        }

        ret = reap_filters(1);
        if (ret < 0 && ret != AVERROR_EOF) {
            av_log(NULL, AV_LOG_ERROR, "Error while filtering: %s\n", av_err2str(ret));
            return ret;
        }

		//printf("name %s\n", fg->graph->filters[i]->name);

        ret = configure_filtergraph(fg);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error reinitializing filters!\n");
            return ret;
        }
    }

    ret = av_buffersrc_add_frame_flags(ifilter->filter, frame, AV_BUFFERSRC_FLAG_PUSH);
    if (ret < 0) {
        if (ret != AVERROR_EOF)
            av_log(NULL, AV_LOG_ERROR, "Error while filtering: %s\n", av_err2str(ret));
        return ret;
    }

    return 0;
}

static int ifilter_send_eof(InputFilter *ifilter, int64_t pts)
{
    int ret;

    ifilter->eof = 1;

    if (ifilter->filter) {
        ret = av_buffersrc_close(ifilter->filter, pts, AV_BUFFERSRC_FLAG_PUSH);
        if (ret < 0)
            return ret;
    } else {
        // the filtergraph was never configured
        if (ifilter->format < 0)
            ifilter_parameters_from_codecpar(ifilter, ifilter->ist->st->codecpar);
        if (ifilter->format < 0 && (ifilter->type == AVMEDIA_TYPE_AUDIO || ifilter->type == AVMEDIA_TYPE_VIDEO)) {
            av_log(NULL, AV_LOG_ERROR, "Cannot determine format of input stream %d:%d after EOF\n", ifilter->ist->file_index, ifilter->ist->st->index);
            return AVERROR_INVALIDDATA;
        }
    }

    return 0;
}

// This does not quite work like avcodec_decode_audio4/avcodec_decode_video2.
// There is the following difference: if you got a frame, you must call
// it again with pkt=NULL. pkt==NULL is treated differently from pkt->size==0
// (pkt==NULL means get more output, pkt->size==0 is a flush/drain packet)
static int decode(AVCodecContext *avctx, AVFrame *frame, int *got_frame, AVPacket *pkt)
{
    int ret;

    *got_frame = 0;

    if (pkt) {
        ret = avcodec_send_packet(avctx, pkt);
        // In particular, we don't expect AVERROR(EAGAIN), because we read all
        // decoded frames with avcodec_receive_frame() until done.
        if (ret < 0 && ret != AVERROR_EOF)
            return ret;
    }

    ret = avcodec_receive_frame(avctx, frame);
    if (ret < 0 && ret != AVERROR(EAGAIN))
        return ret;
    if (ret >= 0)
        *got_frame = 1;

    return 0;
}

static int send_frame_to_filters(InputStream *ist, AVFrame *decoded_frame)
{
    int i, ret;
    AVFrame *f;

    av_assert1(ist->nb_filters > 0); /* ensure ret is initialized */
    for (i = 0; i < ist->nb_filters; i++) {
        if (i < ist->nb_filters - 1) {
            f = ist->filter_frame;
            ret = av_frame_ref(f, decoded_frame);
            if (ret < 0)
                break;
        } else
            f = decoded_frame;
        ret = ifilter_send_frame(ist->filters[i], f);
        if (ret == AVERROR_EOF)
            ret = 0; /* ignore */
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Failed to inject frame into filter network: %s\n", av_err2str(ret));
            break;
        }
    }
    return ret;
}

static int decode_audio(InputStream *ist, AVPacket *pkt, int *got_output,
                        int *decode_failed)
{
    AVFrame *decoded_frame;
    AVCodecContext *avctx = ist->dec_ctx;
    int ret, err = 0;
    AVRational decoded_frame_tb;

    if (!ist->decoded_frame && !(ist->decoded_frame = av_frame_alloc()))
        return AVERROR(ENOMEM);
    if (!ist->filter_frame && !(ist->filter_frame = av_frame_alloc()))
        return AVERROR(ENOMEM);
    decoded_frame = ist->decoded_frame;

    update_benchmark(NULL);
    ret = decode(avctx, decoded_frame, got_output, pkt);
    update_benchmark("decode_audio %d.%d", ist->file_index, ist->st->index);
    if (ret < 0)
        *decode_failed = 1;

    if (ret >= 0 && avctx->sample_rate <= 0) {
        av_log(avctx, AV_LOG_ERROR, "Sample rate %d invalid\n", avctx->sample_rate);
        ret = AVERROR_INVALIDDATA;
    }

    if (ret != AVERROR_EOF)
        check_decode_result(ist, got_output, ret);

    if (!*got_output || ret < 0)
        return ret;

    ist->samples_decoded += decoded_frame->nb_samples;
    ist->frames_decoded++;

    /* increment next_dts to use for the case where the input stream does not
       have timestamps or there are multiple frames in the packet */
    ist->next_pts += ((int64_t)AV_TIME_BASE * decoded_frame->nb_samples) /
                     avctx->sample_rate;
    ist->next_dts += ((int64_t)AV_TIME_BASE * decoded_frame->nb_samples) /
                     avctx->sample_rate;

    if (decoded_frame->pts != AV_NOPTS_VALUE) {
        decoded_frame_tb   = ist->st->time_base;
    } else if (pkt && pkt->pts != AV_NOPTS_VALUE) {
        decoded_frame->pts = pkt->pts;
        decoded_frame_tb   = ist->st->time_base;
    }else {
        decoded_frame->pts = ist->dts;
        decoded_frame_tb   = AV_TIME_BASE_Q;
    }
    if (decoded_frame->pts != AV_NOPTS_VALUE)
        decoded_frame->pts = av_rescale_delta(decoded_frame_tb, decoded_frame->pts,
                                              (AVRational){1, avctx->sample_rate}, decoded_frame->nb_samples, &ist->filter_in_rescale_delta_last,
                                              (AVRational){1, avctx->sample_rate});
    ist->nb_samples = decoded_frame->nb_samples;
    err = send_frame_to_filters(ist, decoded_frame);

    av_frame_unref(ist->filter_frame);
    av_frame_unref(decoded_frame);
    return err < 0 ? err : ret;
}

static int decode_video(InputStream *ist, AVPacket *pkt, int *got_output, int64_t *duration_pts, int eof,
                        int *decode_failed)
{
    AVFrame *decoded_frame;
    int i, ret = 0, err = 0;
    int64_t best_effort_timestamp;
    int64_t dts = AV_NOPTS_VALUE;
    AVPacket avpkt;

    // With fate-indeo3-2, we're getting 0-sized packets before EOF for some
    // reason. This seems like a semi-critical bug. Don't trigger EOF, and
    // skip the packet.
    if (!eof && pkt && pkt->size == 0)
        return 0;

    if (!ist->decoded_frame && !(ist->decoded_frame = av_frame_alloc()))
        return AVERROR(ENOMEM);
    if (!ist->filter_frame && !(ist->filter_frame = av_frame_alloc()))
        return AVERROR(ENOMEM);
    decoded_frame = ist->decoded_frame;
    if (ist->dts != AV_NOPTS_VALUE)
        dts = av_rescale_q(ist->dts, AV_TIME_BASE_Q, ist->st->time_base);
    if (pkt) {
        avpkt = *pkt;
        avpkt.dts = dts; // ffmpeg.c probably shouldn't do this
    }

    // The old code used to set dts on the drain packet, which does not work
    // with the new API anymore.
    if (eof) {
        void *new = av_realloc_array(ist->dts_buffer, ist->nb_dts_buffer + 1, sizeof(ist->dts_buffer[0]));
        if (!new)
            return AVERROR(ENOMEM);
        ist->dts_buffer = new;
        ist->dts_buffer[ist->nb_dts_buffer++] = dts;
    }

    update_benchmark(NULL);
    ret = decode(ist->dec_ctx, decoded_frame, got_output, pkt ? &avpkt : NULL);
    update_benchmark("decode_video %d.%d", ist->file_index, ist->st->index);
    if (ret < 0)
        *decode_failed = 1;

    // The following line may be required in some cases where there is no parser
    // or the parser does not has_b_frames correctly
    if (ist->st->codecpar->video_delay < ist->dec_ctx->has_b_frames) {
        if (ist->dec_ctx->codec_id == AV_CODEC_ID_H264) {
            ist->st->codecpar->video_delay = ist->dec_ctx->has_b_frames;
        } else
            av_log(ist->dec_ctx, AV_LOG_WARNING,
                   "video_delay is larger in decoder than demuxer %d > %d.\n"
                   "If you want to help, upload a sample "
                   "of this file to ftp://upload.ffmpeg.org/incoming/ "
                   "and contact the ffmpeg-devel mailing list. (ffmpeg-devel@ffmpeg.org)\n",
                   ist->dec_ctx->has_b_frames,
                   ist->st->codecpar->video_delay);
    }

    if (ret != AVERROR_EOF)
        check_decode_result(ist, got_output, ret);

    if (*got_output && ret >= 0) {
        if (ist->dec_ctx->width  != decoded_frame->width ||
            ist->dec_ctx->height != decoded_frame->height ||
            ist->dec_ctx->pix_fmt != decoded_frame->format) {
            av_log(NULL, AV_LOG_DEBUG, "Frame parameters mismatch context %d,%d,%d != %d,%d,%d\n",
                decoded_frame->width,
                decoded_frame->height,
                decoded_frame->format,
                ist->dec_ctx->width,
                ist->dec_ctx->height,
                ist->dec_ctx->pix_fmt);
        }
    }

    if (!*got_output || ret < 0)
        return ret;

    if(ist->top_field_first>=0)
        decoded_frame->top_field_first = ist->top_field_first;

    ist->frames_decoded++;

    if (ist->hwaccel_retrieve_data && decoded_frame->format == ist->hwaccel_pix_fmt) {
        err = ist->hwaccel_retrieve_data(ist->dec_ctx, decoded_frame);
        if (err < 0)
            goto fail;
    }
    ist->hwaccel_retrieved_pix_fmt = decoded_frame->format;

    best_effort_timestamp= decoded_frame->best_effort_timestamp;
    *duration_pts = decoded_frame->pkt_duration;

    if (ist->framerate.num)
        best_effort_timestamp = ist->cfr_next_pts++;

    if (eof && best_effort_timestamp == AV_NOPTS_VALUE && ist->nb_dts_buffer > 0) {
        best_effort_timestamp = ist->dts_buffer[0];

        for (i = 0; i < ist->nb_dts_buffer - 1; i++)
            ist->dts_buffer[i] = ist->dts_buffer[i + 1];
        ist->nb_dts_buffer--;
    }

    if(best_effort_timestamp != AV_NOPTS_VALUE) {
        int64_t ts = av_rescale_q(decoded_frame->pts = best_effort_timestamp, ist->st->time_base, AV_TIME_BASE_Q);

        if (ts != AV_NOPTS_VALUE)
            ist->next_pts = ist->pts = ts;
    }

    if (debug_ts) {
        av_log(NULL, AV_LOG_INFO, "decoder -> ist_index:%d type:video "
               "frame_pts:%s frame_pts_time:%s best_effort_ts:%"PRId64" best_effort_ts_time:%s keyframe:%d frame_type:%d time_base:%d/%d\n",
               ist->st->index, av_ts2str(decoded_frame->pts),
               av_ts2timestr(decoded_frame->pts, &ist->st->time_base),
               best_effort_timestamp,
               av_ts2timestr(best_effort_timestamp, &ist->st->time_base),
               decoded_frame->key_frame, decoded_frame->pict_type,
               ist->st->time_base.num, ist->st->time_base.den);
    }

    if (ist->st->sample_aspect_ratio.num)
        decoded_frame->sample_aspect_ratio = ist->st->sample_aspect_ratio;

    err = send_frame_to_filters(ist, decoded_frame);

fail:
    av_frame_unref(ist->filter_frame);
    av_frame_unref(decoded_frame);
    return err < 0 ? err : ret;
}

static int transcode_subtitles(InputStream *ist, AVPacket *pkt, int *got_output,
                               int *decode_failed)
{
    AVSubtitle subtitle;
    int free_sub = 1;
    int i, ret = avcodec_decode_subtitle2(ist->dec_ctx,
                                          &subtitle, got_output, pkt);

    check_decode_result(NULL, got_output, ret);

    if (ret < 0 || !*got_output) {
        *decode_failed = 1;
        if (!pkt->size)
            sub2video_flush(ist);
        return ret;
    }

    if (ist->fix_sub_duration) {
        int end = 1;
        if (ist->prev_sub.got_output) {
            end = av_rescale(subtitle.pts - ist->prev_sub.subtitle.pts,
                             1000, AV_TIME_BASE);
            if (end < ist->prev_sub.subtitle.end_display_time) {
                av_log(ist->dec_ctx, AV_LOG_DEBUG,
                       "Subtitle duration reduced from %"PRId32" to %d%s\n",
                       ist->prev_sub.subtitle.end_display_time, end,
                       end <= 0 ? ", dropping it" : "");
                ist->prev_sub.subtitle.end_display_time = end;
            }
        }
        FFSWAP(int,        *got_output, ist->prev_sub.got_output);
        FFSWAP(int,        ret,         ist->prev_sub.ret);
        FFSWAP(AVSubtitle, subtitle,    ist->prev_sub.subtitle);
        if (end <= 0)
            goto out;
    }

    if (!*got_output)
        return ret;

    if (ist->sub2video.frame) {
        sub2video_update(ist, &subtitle);
    } else if (ist->nb_filters) {
        if (!ist->sub2video.sub_queue)
            ist->sub2video.sub_queue = av_fifo_alloc(8 * sizeof(AVSubtitle));
        if (!ist->sub2video.sub_queue)
            exit_program(1);
        if (!av_fifo_space(ist->sub2video.sub_queue)) {
            ret = av_fifo_realloc2(ist->sub2video.sub_queue, 2 * av_fifo_size(ist->sub2video.sub_queue));
            if (ret < 0)
                exit_program(1);
        }
        av_fifo_generic_write(ist->sub2video.sub_queue, &subtitle, sizeof(subtitle), NULL);
        free_sub = 0;
    }

    if (!subtitle.num_rects)
        goto out;

    ist->frames_decoded++;

    for (i = 0; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];

        if (!check_output_constraints(ist, ost) || !ost->encoding_needed
            || ost->enc->type != AVMEDIA_TYPE_SUBTITLE)
            continue;

        do_subtitle_out(output_files[ost->file_index], ost, &subtitle);
    }

out:
    if (free_sub)
        avsubtitle_free(&subtitle);
    return ret;
}

static int send_filter_eof(InputStream *ist)
{
    int i, ret;
    /* TODO keep pts also in stream time base to avoid converting back */
    int64_t pts = av_rescale_q_rnd(ist->pts, AV_TIME_BASE_Q, ist->st->time_base,
                                   AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);

    for (i = 0; i < ist->nb_filters; i++) {
        ret = ifilter_send_eof(ist->filters[i], pts);
        if (ret < 0)
            return ret;
    }
    return 0;
}

/* pkt = NULL means EOF (needed to flush decoder buffers) */
static int process_input_packet(InputStream *ist, const AVPacket *pkt, int no_eof)
{
    int ret = 0, i;
    int repeating = 0;
    int eof_reached = 0;

    AVPacket avpkt;
    if (!ist->saw_first_ts) {
        ist->dts = ist->st->avg_frame_rate.num ? - ist->dec_ctx->has_b_frames * AV_TIME_BASE / av_q2d(ist->st->avg_frame_rate) : 0;
        ist->pts = 0;
        if (pkt && pkt->pts != AV_NOPTS_VALUE && !ist->decoding_needed) {
            ist->dts += av_rescale_q(pkt->pts, ist->st->time_base, AV_TIME_BASE_Q);
            ist->pts = ist->dts; //unused but better to set it to a value thats not totally wrong
        }
        ist->saw_first_ts = 1;
    }

    if (ist->next_dts == AV_NOPTS_VALUE)
        ist->next_dts = ist->dts;
    if (ist->next_pts == AV_NOPTS_VALUE)
        ist->next_pts = ist->pts;

    if (!pkt) {
        /* EOF handling */
        av_init_packet(&avpkt);
        avpkt.data = NULL;
        avpkt.size = 0;
    } else {
        avpkt = *pkt;
    }

    if (pkt && pkt->dts != AV_NOPTS_VALUE) {
        ist->next_dts = ist->dts = av_rescale_q(pkt->dts, ist->st->time_base, AV_TIME_BASE_Q);
        if (ist->dec_ctx->codec_type != AVMEDIA_TYPE_VIDEO || !ist->decoding_needed)
            ist->next_pts = ist->pts = ist->dts;
    }

    // while we have more to decode or while the decoder did output something on EOF
    while (ist->decoding_needed) {
        int64_t duration_dts = 0;
        int64_t duration_pts = 0;
        int got_output = 0;
        int decode_failed = 0;

        ist->pts = ist->next_pts;
        ist->dts = ist->next_dts;

        switch (ist->dec_ctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            ret = decode_audio    (ist, repeating ? NULL : &avpkt, &got_output,
                                   &decode_failed);
            break;
        case AVMEDIA_TYPE_VIDEO:
            ret = decode_video    (ist, repeating ? NULL : &avpkt, &got_output, &duration_pts, !pkt,
                                   &decode_failed);
            if (!repeating || !pkt || got_output) {
                if (pkt && pkt->duration) {
                    duration_dts = av_rescale_q(pkt->duration, ist->st->time_base, AV_TIME_BASE_Q);
                } else if(ist->dec_ctx->framerate.num != 0 && ist->dec_ctx->framerate.den != 0) {
                    int ticks= av_stream_get_parser(ist->st) ? av_stream_get_parser(ist->st)->repeat_pict+1 : ist->dec_ctx->ticks_per_frame;
                    duration_dts = ((int64_t)AV_TIME_BASE *
                                    ist->dec_ctx->framerate.den * ticks) /
                                    ist->dec_ctx->framerate.num / ist->dec_ctx->ticks_per_frame;
                }

                if(ist->dts != AV_NOPTS_VALUE && duration_dts) {
                    ist->next_dts += duration_dts;
                }else
                    ist->next_dts = AV_NOPTS_VALUE;
            }

            if (got_output) {
                if (duration_pts > 0) {
                    ist->next_pts += av_rescale_q(duration_pts, ist->st->time_base, AV_TIME_BASE_Q);
                } else {
                    ist->next_pts += duration_dts;
                }
            }
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            if (repeating)
                break;
            ret = transcode_subtitles(ist, &avpkt, &got_output, &decode_failed);
            if (!pkt && ret >= 0)
                ret = AVERROR_EOF;
            break;
        default:
            return -1;
        }

        if (ret == AVERROR_EOF) {
            eof_reached = 1;
            break;
        }

        if (ret < 0) {
            if (decode_failed) {
                av_log(NULL, AV_LOG_ERROR, "Error while decoding stream #%d:%d: %s\n",
                       ist->file_index, ist->st->index, av_err2str(ret));
            } else {
                av_log(NULL, AV_LOG_FATAL, "Error while processing the decoded "
                       "data for stream #%d:%d\n", ist->file_index, ist->st->index);
            }
            if (!decode_failed || exit_on_error)
                exit_program(1);
            break;
        }

        if (got_output)
            ist->got_output = 1;

        if (!got_output)
            break;

        // During draining, we might get multiple output frames in this loop.
        // ffmpeg.c does not drain the filter chain on configuration changes,
        // which means if we send multiple frames at once to the filters, and
        // one of those frames changes configuration, the buffered frames will
        // be lost. This can upset certain FATE tests.
        // Decode only 1 frame per call on EOF to appease these FATE tests.
        // The ideal solution would be to rewrite decoding to use the new
        // decoding API in a better way.
        if (!pkt)
            break;

        repeating = 1;
    }

    /* after flushing, send an EOF on all the filter inputs attached to the stream */
    /* except when looping we need to flush but not to send an EOF */
    if (!pkt && ist->decoding_needed && eof_reached && !no_eof) {
        int ret = send_filter_eof(ist);
        if (ret < 0) {
            av_log(NULL, AV_LOG_FATAL, "Error marking filters as finished\n");
            exit_program(1);
        }
    }

    /* handle stream copy */
    if (!ist->decoding_needed && pkt) {
        ist->dts = ist->next_dts;
        switch (ist->dec_ctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            av_assert1(pkt->duration >= 0);
            if (ist->dec_ctx->sample_rate) {
                ist->next_dts += ((int64_t)AV_TIME_BASE * ist->dec_ctx->frame_size) /
                                  ist->dec_ctx->sample_rate;
            } else {
                ist->next_dts += av_rescale_q(pkt->duration, ist->st->time_base, AV_TIME_BASE_Q);
            }
            break;
        case AVMEDIA_TYPE_VIDEO:
            if (ist->framerate.num) {
                // TODO: Remove work-around for c99-to-c89 issue 7
                AVRational time_base_q = AV_TIME_BASE_Q;
                int64_t next_dts = av_rescale_q(ist->next_dts, time_base_q, av_inv_q(ist->framerate));
                ist->next_dts = av_rescale_q(next_dts + 1, av_inv_q(ist->framerate), time_base_q);
            } else if (pkt->duration) {
                ist->next_dts += av_rescale_q(pkt->duration, ist->st->time_base, AV_TIME_BASE_Q);
            } else if(ist->dec_ctx->framerate.num != 0) {
                int ticks= av_stream_get_parser(ist->st) ? av_stream_get_parser(ist->st)->repeat_pict + 1 : ist->dec_ctx->ticks_per_frame;
                ist->next_dts += ((int64_t)AV_TIME_BASE *
                                  ist->dec_ctx->framerate.den * ticks) /
                                  ist->dec_ctx->framerate.num / ist->dec_ctx->ticks_per_frame;
            }
            break;
        }
        ist->pts = ist->dts;
        ist->next_pts = ist->next_dts;
    }
    for (i = 0; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];

        if (!check_output_constraints(ist, ost) || ost->encoding_needed)
            continue;

        do_streamcopy(ist, ost, pkt);
    }

    return !eof_reached;
}

static void print_sdp(void)
{
    char sdp[16384];
    int i;
    int j;
    AVIOContext *sdp_pb;
    AVFormatContext **avc;

    for (i = 0; i < nb_output_files; i++) {
        if (!output_files[i]->header_written)
            return;
    }

    avc = av_malloc_array(nb_output_files, sizeof(*avc));
    if (!avc)
        exit_program(1);
    for (i = 0, j = 0; i < nb_output_files; i++) {
        if (!strcmp(output_files[i]->ctx->oformat->name, "rtp")) {
            avc[j] = output_files[i]->ctx;
            j++;
        }
    }

    if (!j)
        goto fail;

    av_sdp_create(avc, j, sdp, sizeof(sdp));

    if (!sdp_filename) {
        printf("SDP:\n%s\n", sdp);
        fflush(stdout);
    } else {
        if (avio_open2(&sdp_pb, sdp_filename, AVIO_FLAG_WRITE, &int_cb, NULL) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to open sdp file '%s'\n", sdp_filename);
        } else {
            avio_printf(sdp_pb, "SDP:\n%s", sdp);
            avio_closep(&sdp_pb);
            av_freep(&sdp_filename);
        }
    }

fail:
    av_freep(&avc);
}

static enum AVPixelFormat get_format(AVCodecContext *s, const enum AVPixelFormat *pix_fmts)
{
    InputStream *ist = s->opaque;
    const enum AVPixelFormat *p;
    int ret;

    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);
        const AVCodecHWConfig  *config = NULL;
        int i;

        if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
            break;

        if (ist->hwaccel_id == HWACCEL_GENERIC ||
            ist->hwaccel_id == HWACCEL_AUTO) {
            for (i = 0;; i++) {
                config = avcodec_get_hw_config(s->codec, i);
                if (!config)
                    break;
                if (!(config->methods &
                      AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
                    continue;
                if (config->pix_fmt == *p)
                    break;
            }
        }
        if (config) {
            if (config->device_type != ist->hwaccel_device_type) {
                // Different hwaccel offered, ignore.
                continue;
            }

            ret = hwaccel_decode_init(s);
            if (ret < 0) {
                if (ist->hwaccel_id == HWACCEL_GENERIC) {
                    av_log(NULL, AV_LOG_FATAL,
                           "%s hwaccel requested for input stream #%d:%d, "
                           "but cannot be initialized.\n",
                           av_hwdevice_get_type_name(config->device_type),
                           ist->file_index, ist->st->index);
                    return AV_PIX_FMT_NONE;
                }
                continue;
            }
        } else {
            const HWAccel *hwaccel = NULL;
            int i;
            for (i = 0; hwaccels[i].name; i++) {
                if (hwaccels[i].pix_fmt == *p) {
                    hwaccel = &hwaccels[i];
                    break;
                }
            }
            if (!hwaccel) {
                // No hwaccel supporting this pixfmt.
                continue;
            }
            if (hwaccel->id != ist->hwaccel_id) {
                // Does not match requested hwaccel.
                continue;
            }

            ret = hwaccel->init(s);
            if (ret < 0) {
                av_log(NULL, AV_LOG_FATAL,
                       "%s hwaccel requested for input stream #%d:%d, "
                       "but cannot be initialized.\n", hwaccel->name,
                       ist->file_index, ist->st->index);
                return AV_PIX_FMT_NONE;
            }
        }

        if (ist->hw_frames_ctx) {
            s->hw_frames_ctx = av_buffer_ref(ist->hw_frames_ctx);
            if (!s->hw_frames_ctx)
                return AV_PIX_FMT_NONE;
        }

        ist->hwaccel_pix_fmt = *p;
        break;
    }

    return *p;
}

static int get_buffer(AVCodecContext *s, AVFrame *frame, int flags)
{
    InputStream *ist = s->opaque;

    if (ist->hwaccel_get_buffer && frame->format == ist->hwaccel_pix_fmt)
        return ist->hwaccel_get_buffer(s, frame, flags);

    return avcodec_default_get_buffer2(s, frame, flags);
}

static int init_input_stream(int ist_index, char *error, int error_len)
{
    int ret;
    InputStream *ist = input_streams[ist_index];

    if (ist->decoding_needed) {
        AVCodec *codec = ist->dec;
        if (!codec) {
            snprintf(error, error_len, "Decoder (codec %s) not found for input stream #%d:%d",
                    avcodec_get_name(ist->dec_ctx->codec_id), ist->file_index, ist->st->index);
            return AVERROR(EINVAL);
        }

        ist->dec_ctx->opaque                = ist;
        ist->dec_ctx->get_format            = get_format;
        ist->dec_ctx->get_buffer2           = get_buffer;
        ist->dec_ctx->thread_safe_callbacks = 1;

        av_opt_set_int(ist->dec_ctx, "refcounted_frames", 1, 0);
        if (ist->dec_ctx->codec_id == AV_CODEC_ID_DVB_SUBTITLE &&
           (ist->decoding_needed & DECODING_FOR_OST)) {
            av_dict_set(&ist->decoder_opts, "compute_edt", "1", AV_DICT_DONT_OVERWRITE);
            if (ist->decoding_needed & DECODING_FOR_FILTER)
                av_log(NULL, AV_LOG_WARNING, "Warning using DVB subtitles for filtering and output at the same time is not fully supported, also see -compute_edt [0|1]\n");
        }

        av_dict_set(&ist->decoder_opts, "sub_text_format", "ass", AV_DICT_DONT_OVERWRITE);

        /* Useful for subtitles retiming by lavf (FIXME), skipping samples in
         * audio, and video decoders such as cuvid or mediacodec */
        ist->dec_ctx->pkt_timebase = ist->st->time_base;

        if (!av_dict_get(ist->decoder_opts, "threads", NULL, 0))
            av_dict_set(&ist->decoder_opts, "threads", "auto", 0);
        /* Attached pics are sparse, therefore we would not want to delay their decoding till EOF. */
        if (ist->st->disposition & AV_DISPOSITION_ATTACHED_PIC)
            av_dict_set(&ist->decoder_opts, "threads", "1", 0);

        ret = hw_device_setup_for_decode(ist);
        if (ret < 0) {
            snprintf(error, error_len, "Device setup failed for "
                     "decoder on input stream #%d:%d : %s",
                     ist->file_index, ist->st->index, av_err2str(ret));
            return ret;
        }

        if ((ret = avcodec_open2(ist->dec_ctx, codec, &ist->decoder_opts)) < 0) {
            if (ret == AVERROR_EXPERIMENTAL)
                abort_codec_experimental(codec, 0);

            snprintf(error, error_len,
                     "Error while opening decoder for input stream "
                     "#%d:%d : %s",
                     ist->file_index, ist->st->index, av_err2str(ret));
            return ret;
        }
        assert_avoptions(ist->decoder_opts);
    }

    ist->next_pts = AV_NOPTS_VALUE;
    ist->next_dts = AV_NOPTS_VALUE;

    return 0;
}

static InputStream *get_input_stream(OutputStream *ost)
{
    if (ost->source_index >= 0)
        return input_streams[ost->source_index];
    return NULL;
}

static int compare_int64(const void *a, const void *b)
{
    return FFDIFFSIGN(*(const int64_t *)a, *(const int64_t *)b);
}

/* open the muxer when all the streams are initialized */
static int check_init_output_file(OutputFile *of, int file_index)
{
    int ret, i;

    for (i = 0; i < of->ctx->nb_streams; i++) {
        OutputStream *ost = output_streams[of->ost_index + i];
        if (!ost->initialized)
            return 0;
    }

    of->ctx->interrupt_callback = int_cb;

    ret = avformat_write_header(of->ctx, &of->opts);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Could not write header for output file #%d "
               "(incorrect codec parameters ?): %s\n",
               file_index, av_err2str(ret));
        return ret;
    }
    //assert_avoptions(of->opts);
    of->header_written = 1;

    av_dump_format(of->ctx, file_index, of->ctx->url, 1);

    if (sdp_filename || want_sdp)
        print_sdp();

    /* flush the muxing queues */
    for (i = 0; i < of->ctx->nb_streams; i++) {
        OutputStream *ost = output_streams[of->ost_index + i];

        /* try to improve muxing time_base (only possible if nothing has been written yet) */
        if (!av_fifo_size(ost->muxing_queue))
            ost->mux_timebase = ost->st->time_base;

        while (av_fifo_size(ost->muxing_queue)) {
            AVPacket pkt;
            av_fifo_generic_read(ost->muxing_queue, &pkt, sizeof(pkt), NULL);
            write_packet(of, &pkt, ost, 1);
        }
    }

    return 0;
}

static int init_output_bsfs(OutputStream *ost)
{
    AVBSFContext *ctx;
    int i, ret;

    if (!ost->nb_bitstream_filters)
        return 0;

    for (i = 0; i < ost->nb_bitstream_filters; i++) {
        ctx = ost->bsf_ctx[i];

        ret = avcodec_parameters_copy(ctx->par_in,
                                      i ? ost->bsf_ctx[i - 1]->par_out : ost->st->codecpar);
        if (ret < 0)
            return ret;

        ctx->time_base_in = i ? ost->bsf_ctx[i - 1]->time_base_out : ost->st->time_base;

        ret = av_bsf_init(ctx);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error initializing bitstream filter: %s\n",
                   ost->bsf_ctx[i]->filter->name);
            return ret;
        }
    }

    ctx = ost->bsf_ctx[ost->nb_bitstream_filters - 1];
    ret = avcodec_parameters_copy(ost->st->codecpar, ctx->par_out);
    if (ret < 0)
        return ret;

    ost->st->time_base = ctx->time_base_out;

    return 0;
}

static int init_output_stream_streamcopy(OutputStream *ost)
{
    OutputFile *of = output_files[ost->file_index];
    InputStream *ist = get_input_stream(ost);
    AVCodecParameters *par_dst = ost->st->codecpar;
    AVCodecParameters *par_src = ost->ref_par;
    AVRational sar;
    int i, ret;
    uint32_t codec_tag = par_dst->codec_tag;

    av_assert0(ist && !ost->filter);

    ret = avcodec_parameters_to_context(ost->enc_ctx, ist->st->codecpar);
    if (ret >= 0)
        ret = av_opt_set_dict(ost->enc_ctx, &ost->encoder_opts);
    if (ret < 0) {
        av_log(NULL, AV_LOG_FATAL,
               "Error setting up codec context options.\n");
        return ret;
    }

    ret = avcodec_parameters_from_context(par_src, ost->enc_ctx);
    if (ret < 0) {
        av_log(NULL, AV_LOG_FATAL,
               "Error getting reference codec parameters.\n");
        return ret;
    }

    if (!codec_tag) {
        unsigned int codec_tag_tmp;
        if (!of->ctx->oformat->codec_tag ||
            av_codec_get_id (of->ctx->oformat->codec_tag, par_src->codec_tag) == par_src->codec_id ||
            !av_codec_get_tag2(of->ctx->oformat->codec_tag, par_src->codec_id, &codec_tag_tmp))
            codec_tag = par_src->codec_tag;
    }

    ret = avcodec_parameters_copy(par_dst, par_src);
    if (ret < 0)
        return ret;

    par_dst->codec_tag = codec_tag;

    if (!ost->frame_rate.num)
        ost->frame_rate = ist->framerate;
    ost->st->avg_frame_rate = ost->frame_rate;

    ret = avformat_transfer_internal_stream_timing_info(of->ctx->oformat, ost->st, ist->st, copy_tb);
    if (ret < 0)
        return ret;

    // copy timebase while removing common factors
    if (ost->st->time_base.num <= 0 || ost->st->time_base.den <= 0)
        ost->st->time_base = av_add_q(av_stream_get_codec_timebase(ost->st), (AVRational){0, 1});

    // copy estimated duration as a hint to the muxer
    if (ost->st->duration <= 0 && ist->st->duration > 0)
        ost->st->duration = av_rescale_q(ist->st->duration, ist->st->time_base, ost->st->time_base);

    // copy disposition
    ost->st->disposition = ist->st->disposition;

    if (ist->st->nb_side_data) {
        for (i = 0; i < ist->st->nb_side_data; i++) {
            const AVPacketSideData *sd_src = &ist->st->side_data[i];
            uint8_t *dst_data;

            dst_data = av_stream_new_side_data(ost->st, sd_src->type, sd_src->size);
            if (!dst_data)
                return AVERROR(ENOMEM);
            memcpy(dst_data, sd_src->data, sd_src->size);
        }
    }

    if (ost->rotate_overridden) {
        uint8_t *sd = av_stream_new_side_data(ost->st, AV_PKT_DATA_DISPLAYMATRIX,
                                              sizeof(int32_t) * 9);
        if (sd)
            av_display_rotation_set((int32_t *)sd, -ost->rotate_override_value);
    }

    switch (par_dst->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        if (audio_volume != 256) {
            av_log(NULL, AV_LOG_FATAL, "-acodec copy and -vol are incompatible (frames are not decoded)\n");
            exit_program(1);
        }
        if((par_dst->block_align == 1 || par_dst->block_align == 1152 || par_dst->block_align == 576) && par_dst->codec_id == AV_CODEC_ID_MP3)
            par_dst->block_align= 0;
        if(par_dst->codec_id == AV_CODEC_ID_AC3)
            par_dst->block_align= 0;
        break;
    case AVMEDIA_TYPE_VIDEO:
        if (ost->frame_aspect_ratio.num) { // overridden by the -aspect cli option
            sar =
                av_mul_q(ost->frame_aspect_ratio,
                         (AVRational){ par_dst->height, par_dst->width });
            av_log(NULL, AV_LOG_WARNING, "Overriding aspect ratio "
                   "with stream copy may produce invalid files\n");
            }
        else if (ist->st->sample_aspect_ratio.num)
            sar = ist->st->sample_aspect_ratio;
        else
            sar = par_src->sample_aspect_ratio;
        ost->st->sample_aspect_ratio = par_dst->sample_aspect_ratio = sar;
        ost->st->avg_frame_rate = ist->st->avg_frame_rate;
        ost->st->r_frame_rate = ist->st->r_frame_rate;
        break;
    }

    ost->mux_timebase = ist->st->time_base;

    return 0;
}

static void set_encoder_id(OutputFile *of, OutputStream *ost)
{
    AVDictionaryEntry *e;

    uint8_t *encoder_string;
    int encoder_string_len;
    int format_flags = 0;
    int codec_flags = ost->enc_ctx->flags;

    if (av_dict_get(ost->st->metadata, "encoder",  NULL, 0))
        return;

    e = av_dict_get(of->opts, "fflags", NULL, 0);
    if (e) {
        const AVOption *o = av_opt_find(of->ctx, "fflags", NULL, 0, 0);
        if (!o)
            return;
        av_opt_eval_flags(of->ctx, o, e->value, &format_flags);
    }
    e = av_dict_get(ost->encoder_opts, "flags", NULL, 0);
    if (e) {
        const AVOption *o = av_opt_find(ost->enc_ctx, "flags", NULL, 0, 0);
        if (!o)
            return;
        av_opt_eval_flags(ost->enc_ctx, o, e->value, &codec_flags);
    }

    encoder_string_len = sizeof(LIBAVCODEC_IDENT) + strlen(ost->enc->name) + 2;
    encoder_string     = av_mallocz(encoder_string_len);
    if (!encoder_string)
        exit_program(1);

    if (!(format_flags & AVFMT_FLAG_BITEXACT) && !(codec_flags & AV_CODEC_FLAG_BITEXACT))
        av_strlcpy(encoder_string, LIBAVCODEC_IDENT " ", encoder_string_len);
    else
        av_strlcpy(encoder_string, "Lavc ", encoder_string_len);
    av_strlcat(encoder_string, ost->enc->name, encoder_string_len);
    av_dict_set(&ost->st->metadata, "encoder",  encoder_string,
                AV_DICT_DONT_STRDUP_VAL | AV_DICT_DONT_OVERWRITE);
}

static void parse_forced_key_frames(char *kf, OutputStream *ost,
                                    AVCodecContext *avctx)
{
    char *p;
    int n = 1, i, size, index = 0;
    int64_t t, *pts;

    for (p = kf; *p; p++)
        if (*p == ',')
            n++;
    size = n;
    pts = av_malloc_array(size, sizeof(*pts));
    if (!pts) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate forced key frames array.\n");
        exit_program(1);
    }

    p = kf;
    for (i = 0; i < n; i++) {
        char *next = strchr(p, ',');

        if (next)
            *next++ = 0;

        if (!memcmp(p, "chapters", 8)) {

            AVFormatContext *avf = output_files[ost->file_index]->ctx;
            int j;

            if (avf->nb_chapters > INT_MAX - size ||
                !(pts = av_realloc_f(pts, size += avf->nb_chapters - 1,
                                     sizeof(*pts)))) {
                av_log(NULL, AV_LOG_FATAL,
                       "Could not allocate forced key frames array.\n");
                exit_program(1);
            }
            t = p[8] ? parse_time_or_die("force_key_frames", p + 8, 1) : 0;
            t = av_rescale_q(t, AV_TIME_BASE_Q, avctx->time_base);

            for (j = 0; j < avf->nb_chapters; j++) {
                AVChapter *c = avf->chapters[j];
                av_assert1(index < size);
                pts[index++] = av_rescale_q(c->start, c->time_base,
                                            avctx->time_base) + t;
            }

        } else {

            t = parse_time_or_die("force_key_frames", p, 1);
            av_assert1(index < size);
            pts[index++] = av_rescale_q(t, AV_TIME_BASE_Q, avctx->time_base);

        }

        p = next;
    }

    av_assert0(index == size);
    qsort(pts, size, sizeof(*pts), compare_int64);
    ost->forced_kf_count = size;
    ost->forced_kf_pts   = pts;
}

static void init_encoder_time_base(OutputStream *ost, AVRational default_time_base)
{
    InputStream *ist = get_input_stream(ost);
    AVCodecContext *enc_ctx = ost->enc_ctx;
    AVFormatContext *oc;

    if (ost->enc_timebase.num > 0) {
        enc_ctx->time_base = ost->enc_timebase;
        return;
    }

    if (ost->enc_timebase.num < 0) {
        if (ist) {
            enc_ctx->time_base = ist->st->time_base;
            return;
        }

        oc = output_files[ost->file_index]->ctx;
        av_log(oc, AV_LOG_WARNING, "Input stream data not available, using default time base\n");
    }

    enc_ctx->time_base = default_time_base;
}

static int init_output_stream_encode(OutputStream *ost)
{
    InputStream *ist = get_input_stream(ost);
    AVCodecContext *enc_ctx = ost->enc_ctx;
    AVCodecContext *dec_ctx = NULL;
    AVFormatContext *oc = output_files[ost->file_index]->ctx;
    int j, ret;

    set_encoder_id(output_files[ost->file_index], ost);

    // Muxers use AV_PKT_DATA_DISPLAYMATRIX to signal rotation. On the other
    // hand, the legacy API makes demuxers set "rotate" metadata entries,
    // which have to be filtered out to prevent leaking them to output files.
    av_dict_set(&ost->st->metadata, "rotate", NULL, 0);

    if (ist) {
        ost->st->disposition          = ist->st->disposition;

        dec_ctx = ist->dec_ctx;

        enc_ctx->chroma_sample_location = dec_ctx->chroma_sample_location;
    } else {
        for (j = 0; j < oc->nb_streams; j++) {
            AVStream *st = oc->streams[j];
            if (st != ost->st && st->codecpar->codec_type == ost->st->codecpar->codec_type)
                break;
        }
        if (j == oc->nb_streams)
            if (ost->st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ||
                ost->st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                ost->st->disposition = AV_DISPOSITION_DEFAULT;
    }

    if (enc_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        if (!ost->frame_rate.num)
            ost->frame_rate = av_buffersink_get_frame_rate(ost->filter->filter);
        if (ist && !ost->frame_rate.num)
            ost->frame_rate = ist->framerate;
        if (ist && !ost->frame_rate.num)
            ost->frame_rate = ist->st->r_frame_rate;
        if (ist && !ost->frame_rate.num) {
            ost->frame_rate = (AVRational){25, 1};
            av_log(NULL, AV_LOG_WARNING,
                   "No information "
                   "about the input framerate is available. Falling "
                   "back to a default value of 25fps for output stream #%d:%d. Use the -r option "
                   "if you want a different framerate.\n",
                   ost->file_index, ost->index);
        }

        if (ost->enc->supported_framerates && !ost->force_fps) {
            int idx = av_find_nearest_q_idx(ost->frame_rate, ost->enc->supported_framerates);
            ost->frame_rate = ost->enc->supported_framerates[idx];
        }
        // reduce frame rate for mpeg4 to be within the spec limits
        if (enc_ctx->codec_id == AV_CODEC_ID_MPEG4) {
            av_reduce(&ost->frame_rate.num, &ost->frame_rate.den,
                      ost->frame_rate.num, ost->frame_rate.den, 65535);
        }
    }

    switch (enc_ctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        enc_ctx->sample_fmt     = av_buffersink_get_format(ost->filter->filter);
        if (dec_ctx)
            enc_ctx->bits_per_raw_sample = FFMIN(dec_ctx->bits_per_raw_sample,
                                                 av_get_bytes_per_sample(enc_ctx->sample_fmt) << 3);
        enc_ctx->sample_rate    = av_buffersink_get_sample_rate(ost->filter->filter);
        enc_ctx->channel_layout = av_buffersink_get_channel_layout(ost->filter->filter);
        enc_ctx->channels       = av_buffersink_get_channels(ost->filter->filter);

        init_encoder_time_base(ost, av_make_q(1, enc_ctx->sample_rate));
        break;

    case AVMEDIA_TYPE_VIDEO:
        init_encoder_time_base(ost, av_inv_q(ost->frame_rate));

        if (!(enc_ctx->time_base.num && enc_ctx->time_base.den))
            enc_ctx->time_base = av_buffersink_get_time_base(ost->filter->filter);
        if (   av_q2d(enc_ctx->time_base) < 0.001 && video_sync_method != VSYNC_PASSTHROUGH
           && (video_sync_method == VSYNC_CFR || video_sync_method == VSYNC_VSCFR || (video_sync_method == VSYNC_AUTO && !(oc->oformat->flags & AVFMT_VARIABLE_FPS)))){
            av_log(oc, AV_LOG_WARNING, "Frame rate very high for a muxer not efficiently supporting it.\n"
                                       "Please consider specifying a lower framerate, a different muxer or -vsync 2\n");
        }
        for (j = 0; j < ost->forced_kf_count; j++)
            ost->forced_kf_pts[j] = av_rescale_q(ost->forced_kf_pts[j],
                                                 AV_TIME_BASE_Q,
                                                 enc_ctx->time_base);

        enc_ctx->width  = av_buffersink_get_w(ost->filter->filter);
        enc_ctx->height = av_buffersink_get_h(ost->filter->filter);
        enc_ctx->sample_aspect_ratio = ost->st->sample_aspect_ratio =
            ost->frame_aspect_ratio.num ? // overridden by the -aspect cli option
            av_mul_q(ost->frame_aspect_ratio, (AVRational){ enc_ctx->height, enc_ctx->width }) :
            av_buffersink_get_sample_aspect_ratio(ost->filter->filter);

        enc_ctx->pix_fmt = av_buffersink_get_format(ost->filter->filter);
        if (dec_ctx)
            enc_ctx->bits_per_raw_sample = FFMIN(dec_ctx->bits_per_raw_sample,
                                                 av_pix_fmt_desc_get(enc_ctx->pix_fmt)->comp[0].depth);

        enc_ctx->framerate = ost->frame_rate;

        ost->st->avg_frame_rate = ost->frame_rate;

        if (!dec_ctx ||
            enc_ctx->width   != dec_ctx->width  ||
            enc_ctx->height  != dec_ctx->height ||
            enc_ctx->pix_fmt != dec_ctx->pix_fmt) {
            enc_ctx->bits_per_raw_sample = frame_bits_per_raw_sample;
        }

        if (ost->top_field_first == 0) {
            enc_ctx->field_order = AV_FIELD_BB;
        } else if (ost->top_field_first == 1) {
            enc_ctx->field_order = AV_FIELD_TT;
        }

        if (ost->forced_keyframes) {
            if (!strncmp(ost->forced_keyframes, "expr:", 5)) {
                ret = av_expr_parse(&ost->forced_keyframes_pexpr, ost->forced_keyframes+5,
                                    forced_keyframes_const_names, NULL, NULL, NULL, NULL, 0, NULL);
                if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR,
                           "Invalid force_key_frames expression '%s'\n", ost->forced_keyframes+5);
                    return ret;
                }
                ost->forced_keyframes_expr_const_values[FKF_N] = 0;
                ost->forced_keyframes_expr_const_values[FKF_N_FORCED] = 0;
                ost->forced_keyframes_expr_const_values[FKF_PREV_FORCED_N] = NAN;
                ost->forced_keyframes_expr_const_values[FKF_PREV_FORCED_T] = NAN;

                // Don't parse the 'forced_keyframes' in case of 'keep-source-keyframes',
                // parse it only for static kf timings
            } else if(strncmp(ost->forced_keyframes, "source", 6)) {
                parse_forced_key_frames(ost->forced_keyframes, ost, ost->enc_ctx);
            }
        }
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        enc_ctx->time_base = AV_TIME_BASE_Q;
        if (!enc_ctx->width) {
            enc_ctx->width     = input_streams[ost->source_index]->st->codecpar->width;
            enc_ctx->height    = input_streams[ost->source_index]->st->codecpar->height;
        }
        break;
    case AVMEDIA_TYPE_DATA:
        break;
    default:
        abort();
        break;
    }

    ost->mux_timebase = enc_ctx->time_base;

    return 0;
}

static int init_output_stream(OutputStream *ost, char *error, int error_len)
{
    int ret = 0;

    if (ost->encoding_needed) {
        AVCodec      *codec = ost->enc;
        AVCodecContext *dec = NULL;
        InputStream *ist;

        ret = init_output_stream_encode(ost);
        if (ret < 0)
            return ret;

        if ((ist = get_input_stream(ost)))
            dec = ist->dec_ctx;
        if (dec && dec->subtitle_header) {
            /* ASS code assumes this buffer is null terminated so add extra byte. */
            ost->enc_ctx->subtitle_header = av_mallocz(dec->subtitle_header_size + 1);
            if (!ost->enc_ctx->subtitle_header)
                return AVERROR(ENOMEM);
            memcpy(ost->enc_ctx->subtitle_header, dec->subtitle_header, dec->subtitle_header_size);
            ost->enc_ctx->subtitle_header_size = dec->subtitle_header_size;
        }
        if (!av_dict_get(ost->encoder_opts, "threads", NULL, 0))
            av_dict_set(&ost->encoder_opts, "threads", "auto", 0);
        if (ost->enc->type == AVMEDIA_TYPE_AUDIO &&
            !codec->defaults &&
            !av_dict_get(ost->encoder_opts, "b", NULL, 0) &&
            !av_dict_get(ost->encoder_opts, "ab", NULL, 0))
            av_dict_set(&ost->encoder_opts, "b", "128000", 0);

        if (ost->filter && av_buffersink_get_hw_frames_ctx(ost->filter->filter) &&
            ((AVHWFramesContext*)av_buffersink_get_hw_frames_ctx(ost->filter->filter)->data)->format ==
            av_buffersink_get_format(ost->filter->filter)) {
            ost->enc_ctx->hw_frames_ctx = av_buffer_ref(av_buffersink_get_hw_frames_ctx(ost->filter->filter));
            if (!ost->enc_ctx->hw_frames_ctx)
                return AVERROR(ENOMEM);
        } else {
            ret = hw_device_setup_for_encode(ost);
            if (ret < 0) {
                snprintf(error, error_len, "Device setup failed for "
                         "encoder on output stream #%d:%d : %s",
                     ost->file_index, ost->index, av_err2str(ret));
                return ret;
            }
        }
        if (ist && ist->dec->type == AVMEDIA_TYPE_SUBTITLE && ost->enc->type == AVMEDIA_TYPE_SUBTITLE) {
            int input_props = 0, output_props = 0;
            AVCodecDescriptor const *input_descriptor =
                avcodec_descriptor_get(dec->codec_id);
            AVCodecDescriptor const *output_descriptor =
                avcodec_descriptor_get(ost->enc_ctx->codec_id);
            if (input_descriptor)
                input_props = input_descriptor->props & (AV_CODEC_PROP_TEXT_SUB | AV_CODEC_PROP_BITMAP_SUB);
            if (output_descriptor)
                output_props = output_descriptor->props & (AV_CODEC_PROP_TEXT_SUB | AV_CODEC_PROP_BITMAP_SUB);
            if (input_props && output_props && input_props != output_props) {
                snprintf(error, error_len,
                         "Subtitle encoding currently only possible from text to text "
                         "or bitmap to bitmap");
                return AVERROR_INVALIDDATA;
            }
        }

        if ((ret = avcodec_open2(ost->enc_ctx, codec, &ost->encoder_opts)) < 0) {
            if (ret == AVERROR_EXPERIMENTAL)
                abort_codec_experimental(codec, 1);
            snprintf(error, error_len,
                     "Error while opening encoder for output stream #%d:%d - "
                     "maybe incorrect parameters such as bit_rate, rate, width or height",
                    ost->file_index, ost->index);
            return ret;
        }
        if (ost->enc->type == AVMEDIA_TYPE_AUDIO &&
            !(ost->enc->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE))
            av_buffersink_set_frame_size(ost->filter->filter,
                                            ost->enc_ctx->frame_size);
        assert_avoptions(ost->encoder_opts);
        if (ost->enc_ctx->bit_rate && ost->enc_ctx->bit_rate < 1000 &&
            ost->enc_ctx->codec_id != AV_CODEC_ID_CODEC2 /* don't complain about 700 bit/s modes */)
            av_log(NULL, AV_LOG_WARNING, "The bitrate parameter is set too low."
                                         " It takes bits/s as argument, not kbits/s\n");

        ret = avcodec_parameters_from_context(ost->st->codecpar, ost->enc_ctx);
        if (ret < 0) {
            av_log(NULL, AV_LOG_FATAL,
                   "Error initializing the output stream codec context.\n");
            exit_program(1);
        }
        /*
         * FIXME: ost->st->codec should't be needed here anymore.
         */
        ret = avcodec_copy_context(ost->st->codec, ost->enc_ctx);
        if (ret < 0)
            return ret;

        if (ost->enc_ctx->nb_coded_side_data) {
            int i;

            for (i = 0; i < ost->enc_ctx->nb_coded_side_data; i++) {
                const AVPacketSideData *sd_src = &ost->enc_ctx->coded_side_data[i];
                uint8_t *dst_data;

                dst_data = av_stream_new_side_data(ost->st, sd_src->type, sd_src->size);
                if (!dst_data)
                    return AVERROR(ENOMEM);
                memcpy(dst_data, sd_src->data, sd_src->size);
            }
        }

        /*
         * Add global input side data. For now this is naive, and copies it
         * from the input stream's global side data. All side data should
         * really be funneled over AVFrame and libavfilter, then added back to
         * packet side data, and then potentially using the first packet for
         * global side data.
         */
        if (ist) {
            int i;
            for (i = 0; i < ist->st->nb_side_data; i++) {
                AVPacketSideData *sd = &ist->st->side_data[i];
                uint8_t *dst = av_stream_new_side_data(ost->st, sd->type, sd->size);
                if (!dst)
                    return AVERROR(ENOMEM);
                memcpy(dst, sd->data, sd->size);
                if (ist->autorotate && sd->type == AV_PKT_DATA_DISPLAYMATRIX)
                    av_display_rotation_set((uint32_t *)dst, 0);
            }
        }

        // copy timebase while removing common factors
        if (ost->st->time_base.num <= 0 || ost->st->time_base.den <= 0)
            ost->st->time_base = av_add_q(ost->enc_ctx->time_base, (AVRational){0, 1});

        // copy estimated duration as a hint to the muxer
        if (ost->st->duration <= 0 && ist && ist->st->duration > 0)
            ost->st->duration = av_rescale_q(ist->st->duration, ist->st->time_base, ost->st->time_base);

        ost->st->codec->codec= ost->enc_ctx->codec;
    } else if (ost->stream_copy) {
        ret = init_output_stream_streamcopy(ost);
        if (ret < 0)
            return ret;
    }

    // parse user provided disposition, and update stream values
    if (ost->disposition) {
        static const AVOption opts[] = {
            { "disposition"         , NULL, 0, AV_OPT_TYPE_FLAGS, { .i64 = 0 }, INT64_MIN, INT64_MAX, .unit = "flags" },
            { "default"             , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_DEFAULT           },    .unit = "flags" },
            { "dub"                 , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_DUB               },    .unit = "flags" },
            { "original"            , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_ORIGINAL          },    .unit = "flags" },
            { "comment"             , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_COMMENT           },    .unit = "flags" },
            { "lyrics"              , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_LYRICS            },    .unit = "flags" },
            { "karaoke"             , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_KARAOKE           },    .unit = "flags" },
            { "forced"              , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_FORCED            },    .unit = "flags" },
            { "hearing_impaired"    , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_HEARING_IMPAIRED  },    .unit = "flags" },
            { "visual_impaired"     , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_VISUAL_IMPAIRED   },    .unit = "flags" },
            { "clean_effects"       , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_CLEAN_EFFECTS     },    .unit = "flags" },
            { "attached_pic"        , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_ATTACHED_PIC      },    .unit = "flags" },
            { "captions"            , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_CAPTIONS          },    .unit = "flags" },
            { "descriptions"        , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_DESCRIPTIONS      },    .unit = "flags" },
            { "dependent"           , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_DEPENDENT         },    .unit = "flags" },
            { "metadata"            , NULL, 0, AV_OPT_TYPE_CONST, { .i64 = AV_DISPOSITION_METADATA          },    .unit = "flags" },
            { NULL },
        };
        static const AVClass class = {
            .class_name = "",
            .item_name  = av_default_item_name,
            .option     = opts,
            .version    = LIBAVUTIL_VERSION_INT,
        };
        const AVClass *pclass = &class;

        ret = av_opt_eval_flags(&pclass, &opts[0], ost->disposition, &ost->st->disposition);
        if (ret < 0)
            return ret;
    }

    /* initialize bitstream filters for the output stream
     * needs to be done here, because the codec id for streamcopy is not
     * known until now */
    ret = init_output_bsfs(ost);
    if (ret < 0)
        return ret;

    ost->initialized = 1;

    ret = check_init_output_file(output_files[ost->file_index], ost->file_index);
    if (ret < 0)
        return ret;

    return ret;
}

static void report_new_stream(int input_index, AVPacket *pkt)
{
    InputFile *file = input_files[input_index];
    AVStream *st = file->ctx->streams[pkt->stream_index];

    if (pkt->stream_index < file->nb_streams_warn)
        return;
    av_log(file->ctx, AV_LOG_WARNING,
           "New %s stream %d:%d at pos:%"PRId64" and DTS:%ss\n",
           av_get_media_type_string(st->codecpar->codec_type),
           input_index, pkt->stream_index,
           pkt->pos, av_ts2timestr(pkt->dts, &st->time_base));
    file->nb_streams_warn = pkt->stream_index + 1;
}

static int transcode_init(void)
{
    int ret = 0, i, j, k;
    AVFormatContext *oc;
    OutputStream *ost;
    InputStream *ist;
    char error[1024] = {0};

    for (i = 0; i < nb_filtergraphs; i++) {
        FilterGraph *fg = filtergraphs[i];
        for (j = 0; j < fg->nb_outputs; j++) {
            OutputFilter *ofilter = fg->outputs[j];
            if (!ofilter->ost || ofilter->ost->source_index >= 0)
                continue;
            if (fg->nb_inputs != 1)
                continue;
            for (k = nb_input_streams-1; k >= 0 ; k--)
                if (fg->inputs[0]->ist == input_streams[k])
                    break;
            ofilter->ost->source_index = k;
        }
    }

    /* init framerate emulation */
    for (i = 0; i < nb_input_files; i++) {
        InputFile *ifile = input_files[i];
        if (ifile->rate_emu)
            for (j = 0; j < ifile->nb_streams; j++)
                input_streams[j + ifile->ist_index]->start = av_gettime_relative();
    }

    /* init input streams */
    for (i = 0; i < nb_input_streams; i++)
        if ((ret = init_input_stream(i, error, sizeof(error))) < 0) {
            for (i = 0; i < nb_output_streams; i++) {
                ost = output_streams[i];
                avcodec_close(ost->enc_ctx);
            }
            goto dump_format;
        }

    /* open each encoder */
    for (i = 0; i < nb_output_streams; i++) {
        // skip streams fed from filtergraphs until we have a frame for them
        if (output_streams[i]->filter)
            continue;

        ret = init_output_stream(output_streams[i], error, sizeof(error));
        if (ret < 0)
            goto dump_format;
    }

    /* discard unused programs */
    for (i = 0; i < nb_input_files; i++) {
        InputFile *ifile = input_files[i];
        for (j = 0; j < ifile->ctx->nb_programs; j++) {
            AVProgram *p = ifile->ctx->programs[j];
            int discard  = AVDISCARD_ALL;

            for (k = 0; k < p->nb_stream_indexes; k++)
                if (!input_streams[ifile->ist_index + p->stream_index[k]]->discard) {
                    discard = AVDISCARD_DEFAULT;
                    break;
                }
            p->discard = discard;
        }
    }

    /* write headers for files with no streams */
    for (i = 0; i < nb_output_files; i++) {
        oc = output_files[i]->ctx;
        if (oc->oformat->flags & AVFMT_NOSTREAMS && oc->nb_streams == 0) {
            ret = check_init_output_file(output_files[i], i);
            if (ret < 0)
                goto dump_format;
        }
    }

 dump_format:
    /* dump the stream mapping */
    av_log(NULL, AV_LOG_INFO, "Stream mapping:\n");
    for (i = 0; i < nb_input_streams; i++) {
        ist = input_streams[i];

        for (j = 0; j < ist->nb_filters; j++) {
            if (!filtergraph_is_simple(ist->filters[j]->graph)) {
                av_log(NULL, AV_LOG_INFO, "  Stream #%d:%d (%s) -> %s",
                       ist->file_index, ist->st->index, ist->dec ? ist->dec->name : "?",
                       ist->filters[j]->name);
                if (nb_filtergraphs > 1)
                    av_log(NULL, AV_LOG_INFO, " (graph %d)", ist->filters[j]->graph->index);
                av_log(NULL, AV_LOG_INFO, "\n");
            }
        }
    }

    for (i = 0; i < nb_output_streams; i++) {
        ost = output_streams[i];

        if (ost->attachment_filename) {
            /* an attached file */
            av_log(NULL, AV_LOG_INFO, "  File %s -> Stream #%d:%d\n",
                   ost->attachment_filename, ost->file_index, ost->index);
            continue;
        }

        if (ost->filter && !filtergraph_is_simple(ost->filter->graph)) {
            /* output from a complex graph */
            av_log(NULL, AV_LOG_INFO, "  %s", ost->filter->name);
            if (nb_filtergraphs > 1)
                av_log(NULL, AV_LOG_INFO, " (graph %d)", ost->filter->graph->index);

            av_log(NULL, AV_LOG_INFO, " -> Stream #%d:%d (%s)\n", ost->file_index,
                   ost->index, ost->enc ? ost->enc->name : "?");
            continue;
        }

        av_log(NULL, AV_LOG_INFO, "  Stream #%d:%d -> #%d:%d",
               input_streams[ost->source_index]->file_index,
               input_streams[ost->source_index]->st->index,
               ost->file_index,
               ost->index);
        if (ost->sync_ist != input_streams[ost->source_index])
            av_log(NULL, AV_LOG_INFO, " [sync #%d:%d]",
                   ost->sync_ist->file_index,
                   ost->sync_ist->st->index);
        if (ost->stream_copy)
            av_log(NULL, AV_LOG_INFO, " (copy)");
        else {
            const AVCodec *in_codec    = input_streams[ost->source_index]->dec;
            const AVCodec *out_codec   = ost->enc;
            const char *decoder_name   = "?";
            const char *in_codec_name  = "?";
            const char *encoder_name   = "?";
            const char *out_codec_name = "?";
            const AVCodecDescriptor *desc;

            if (in_codec) {
                decoder_name  = in_codec->name;
                desc = avcodec_descriptor_get(in_codec->id);
                if (desc)
                    in_codec_name = desc->name;
                if (!strcmp(decoder_name, in_codec_name))
                    decoder_name = "native";
            }

            if (out_codec) {
                encoder_name   = out_codec->name;
                desc = avcodec_descriptor_get(out_codec->id);
                if (desc)
                    out_codec_name = desc->name;
                if (!strcmp(encoder_name, out_codec_name))
                    encoder_name = "native";
            }

            av_log(NULL, AV_LOG_INFO, " (%s (%s) -> %s (%s))",
                   in_codec_name, decoder_name,
                   out_codec_name, encoder_name);
        }
        av_log(NULL, AV_LOG_INFO, "\n");
    }

    if (ret) {
        av_log(NULL, AV_LOG_ERROR, "%s\n", error);
        return ret;
    }

    atomic_store(&transcode_init_done, 1);

    return 0;
}

/* Return 1 if there remain streams where more output is wanted, 0 otherwise. */
static int need_output(void)
{
    int i;

    for (i = 0; i < nb_output_streams; i++) {
        OutputStream *ost    = output_streams[i];
        OutputFile *of       = output_files[ost->file_index];
        AVFormatContext *os  = output_files[ost->file_index]->ctx;

        if (ost->finished ||
            (os->pb && avio_tell(os->pb) >= of->limit_filesize))
            continue;
        if (ost->frame_number >= ost->max_frames) {
            int j;
            for (j = 0; j < of->ctx->nb_streams; j++)
                close_output_stream(output_streams[of->ost_index + j]);
            continue;
        }

        return 1;
    }

    return 0;
}

/**
 * Select the output stream to process.
 *
 * @return  selected output stream, or NULL if none available
 */
static OutputStream *choose_output(void)
{
    int i;
    int64_t opts_min = INT64_MAX;
    OutputStream *ost_min = NULL;

    for (i = 0; i < nb_output_streams; i++) {
        OutputStream *ost = output_streams[i];
        int64_t opts = ost->st->cur_dts == AV_NOPTS_VALUE ? INT64_MIN :
                       av_rescale_q(ost->st->cur_dts, ost->st->time_base,
                                    AV_TIME_BASE_Q);
        if (ost->st->cur_dts == AV_NOPTS_VALUE)
            av_log(NULL, AV_LOG_DEBUG,
                "cur_dts is invalid st:%d (%d) [init:%d i_done:%d finish:%d] (this is harmless if it occurs once at the start per stream)\n",
                ost->st->index, ost->st->id, ost->initialized, ost->inputs_done, ost->finished);

        if (!ost->initialized && !ost->inputs_done)
            return ost;

        if (!ost->finished && opts < opts_min) {
            opts_min = opts;
            ost_min  = ost->unavailable ? NULL : ost;
        }
    }
    return ost_min;
}

static void set_tty_echo(int on)
{
#if HAVE_TERMIOS_H
    struct termios tty;
    if (tcgetattr(0, &tty) == 0) {
        if (on) tty.c_lflag |= ECHO;
        else    tty.c_lflag &= ~ECHO;
        tcsetattr(0, TCSANOW, &tty);
    }
#endif
}

static int check_keyboard_interaction(int64_t cur_time)
{
    int i, ret, key;
    static int64_t last_time;
    if (received_nb_signals)
        return AVERROR_EXIT;
    /* read_key() returns 0 on EOF */
    if(cur_time - last_time >= 100000 && !run_as_daemon){
        key =  read_key();
        last_time = cur_time;
    }else
        key = -1;
    if (key == 'q')
        return AVERROR_EXIT;
    if (key == '+') av_log_set_level(av_log_get_level()+10);
    if (key == '-') av_log_set_level(av_log_get_level()-10);
    if (key == 's') qp_hist     ^= 1;
    if (key == 'h'){
        if (do_hex_dump){
            do_hex_dump = do_pkt_dump = 0;
        } else if(do_pkt_dump){
            do_hex_dump = 1;
        } else
            do_pkt_dump = 1;
        av_log_set_level(AV_LOG_DEBUG);
    }
    if (key == 'c' || key == 'C'){
        char buf[4096], target[64], command[256], arg[256] = {0};
        double time;
        int k, n = 0;
        fprintf(stderr, "\nEnter command: <target>|all <time>|-1 <command>[ <argument>]\n");
        i = 0;
        set_tty_echo(1);
        while ((k = read_key()) != '\n' && k != '\r' && i < sizeof(buf)-1)
            if (k > 0)
                buf[i++] = k;
        buf[i] = 0;
        set_tty_echo(0);
        fprintf(stderr, "\n");
        if (k > 0 &&
            (n = sscanf(buf, "%63[^ ] %lf %255[^ ] %255[^\n]", target, &time, command, arg)) >= 3) {
            av_log(NULL, AV_LOG_DEBUG, "Processing command target:%s time:%f command:%s arg:%s",
                   target, time, command, arg);
            for (i = 0; i < nb_filtergraphs; i++) {
                FilterGraph *fg = filtergraphs[i];
                if (fg->graph) {
                    if (time < 0) {
                        ret = avfilter_graph_send_command(fg->graph, target, command, arg, buf, sizeof(buf),
                                                          key == 'c' ? AVFILTER_CMD_FLAG_ONE : 0);
                        fprintf(stderr, "Command reply for stream %d: ret:%d res:\n%s", i, ret, buf);
                    } else if (key == 'c') {
                        fprintf(stderr, "Queuing commands only on filters supporting the specific command is unsupported\n");
                        ret = AVERROR_PATCHWELCOME;
                    } else {
                        ret = avfilter_graph_queue_command(fg->graph, target, command, arg, 0, time);
                        if (ret < 0)
                            fprintf(stderr, "Queuing command failed with error %s\n", av_err2str(ret));
                    }
                }
            }
        } else {
            av_log(NULL, AV_LOG_ERROR,
                   "Parse error, at least 3 arguments were expected, "
                   "only %d given in string '%s'\n", n, buf);
        }
    }
    if (key == 'd' || key == 'D'){
        int debug=0;
        if(key == 'D') {
            debug = input_streams[0]->st->codec->debug<<1;
            if(!debug) debug = 1;
            while(debug & (FF_DEBUG_DCT_COEFF
#if FF_API_DEBUG_MV
                                             |FF_DEBUG_VIS_QP|FF_DEBUG_VIS_MB_TYPE
#endif
                                                                                  )) //unsupported, would just crash
                debug += debug;
        }else{
            char buf[32];
            int k = 0;
            i = 0;
            set_tty_echo(1);
            while ((k = read_key()) != '\n' && k != '\r' && i < sizeof(buf)-1)
                if (k > 0)
                    buf[i++] = k;
            buf[i] = 0;
            set_tty_echo(0);
            fprintf(stderr, "\n");
            if (k <= 0 || sscanf(buf, "%d", &debug)!=1)
                fprintf(stderr,"error parsing debug value\n");
        }
        for(i=0;i<nb_input_streams;i++) {
            input_streams[i]->st->codec->debug = debug;
        }
        for(i=0;i<nb_output_streams;i++) {
            OutputStream *ost = output_streams[i];
            ost->enc_ctx->debug = debug;
        }
        if(debug) av_log_set_level(AV_LOG_DEBUG);
        fprintf(stderr,"debug=%d\n", debug);
    }
    if (key == '?'){
        fprintf(stderr, "key    function\n"
                        "?      show this help\n"
                        "+      increase verbosity\n"
                        "-      decrease verbosity\n"
                        "c      Send command to first matching filter supporting it\n"
                        "C      Send/Queue command to all matching filters\n"
                        "D      cycle through available debug modes\n"
                        "h      dump packets/hex press to cycle through the 3 states\n"
                        "q      quit\n"
                        "s      Show QP histogram\n"
        );
    }
    return 0;
}

#if HAVE_THREADS
static void *input_thread(void *arg)
{
    InputFile *f = arg;
    unsigned flags = f->non_blocking ? AV_THREAD_MESSAGE_NONBLOCK : 0;
    int ret = 0;

    while (1) {
        AVPacket pkt;
        ret = av_read_frame(f->ctx, &pkt);

        if (ret == AVERROR(EAGAIN)) {
            av_usleep(10000);
            continue;
        }
        if (ret < 0) {
            av_thread_message_queue_set_err_recv(f->in_thread_queue, ret);
            break;
        }
        ret = av_thread_message_queue_send(f->in_thread_queue, &pkt, flags);
        if (flags && ret == AVERROR(EAGAIN)) {
            flags = 0;
            ret = av_thread_message_queue_send(f->in_thread_queue, &pkt, flags);
            av_log(f->ctx, AV_LOG_WARNING,
                   "Thread message queue blocking; consider raising the "
                   "thread_queue_size option (current value: %d)\n",
                   f->thread_queue_size);
        }
        if (ret < 0) {
            if (ret != AVERROR_EOF)
                av_log(f->ctx, AV_LOG_ERROR,
                       "Unable to send packet to main thread: %s\n",
                       av_err2str(ret));
            av_packet_unref(&pkt);
            av_thread_message_queue_set_err_recv(f->in_thread_queue, ret);
            break;
        }
    }

    return NULL;
}

static void free_input_thread(int i)
{
    InputFile *f = input_files[i];
    AVPacket pkt;

    if (!f || !f->in_thread_queue)
        return;
    av_thread_message_queue_set_err_send(f->in_thread_queue, AVERROR_EOF);
    while (av_thread_message_queue_recv(f->in_thread_queue, &pkt, 0) >= 0)
        av_packet_unref(&pkt);

    pthread_join(f->thread, NULL);
    f->joined = 1;
    av_thread_message_queue_free(&f->in_thread_queue);
}

static void free_input_threads(void)
{
    int i;

    for (i = 0; i < nb_input_files; i++)
        free_input_thread(i);
}

static int init_input_thread(int i)
{
    int ret;
    InputFile *f = input_files[i];

    if (nb_input_files == 1)
        return 0;

    if (f->ctx->pb ? !f->ctx->pb->seekable :
        strcmp(f->ctx->iformat->name, "lavfi"))
        f->non_blocking = 1;
    ret = av_thread_message_queue_alloc(&f->in_thread_queue,
                                        f->thread_queue_size, sizeof(AVPacket));
    if (ret < 0)
        return ret;

    if ((ret = pthread_create(&f->thread, NULL, input_thread, f))) {
        av_log(NULL, AV_LOG_ERROR, "pthread_create failed: %s. Try to increase `ulimit -v` or decrease `ulimit -s`.\n", strerror(ret));
        av_thread_message_queue_free(&f->in_thread_queue);
        return AVERROR(ret);
    }

    return 0;
}

static int init_input_threads(void)
{
    int i, ret;

    for (i = 0; i < nb_input_files; i++) {
        ret = init_input_thread(i);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int get_input_packet_mt(InputFile *f, AVPacket *pkt)
{
    return av_thread_message_queue_recv(f->in_thread_queue, pkt,
                                        f->non_blocking ?
                                        AV_THREAD_MESSAGE_NONBLOCK : 0);
}
#endif

static int get_input_packet(InputFile *f, AVPacket *pkt)
{
    if (f->rate_emu) {
        int i;
        for (i = 0; i < f->nb_streams; i++) {
            InputStream *ist = input_streams[f->ist_index + i];
            int64_t pts = av_rescale(ist->dts, 1000000, AV_TIME_BASE);
            int64_t now = av_gettime_relative() - ist->start;
            if (pts > now)
                return AVERROR(EAGAIN);
        }
    }

#if HAVE_THREADS
    if (nb_input_files > 1)
        return get_input_packet_mt(f, pkt);
#endif
    return av_read_frame(f->ctx, pkt);
}

static int got_eagain(void)
{
    int i;
    for (i = 0; i < nb_output_streams; i++)
        if (output_streams[i]->unavailable)
            return 1;
    return 0;
}

static void reset_eagain(void)
{
    int i;
    for (i = 0; i < nb_input_files; i++)
        input_files[i]->eagain = 0;
    for (i = 0; i < nb_output_streams; i++)
        output_streams[i]->unavailable = 0;
}

// set duration to max(tmp, duration) in a proper time base and return duration's time_base
static AVRational duration_max(int64_t tmp, int64_t *duration, AVRational tmp_time_base,
                               AVRational time_base)
{
    int ret;

    if (!*duration) {
        *duration = tmp;
        return tmp_time_base;
    }

    ret = av_compare_ts(*duration, time_base, tmp, tmp_time_base);
    if (ret < 0) {
        *duration = tmp;
        return tmp_time_base;
    }

    return time_base;
}

static int seek_to_start(InputFile *ifile, AVFormatContext *is)
{
    InputStream *ist;
    AVCodecContext *avctx;
    int i, ret, has_audio = 0;
    int64_t duration = 0;

    ret = avformat_seek_file(is, -1, INT64_MIN, is->start_time, is->start_time, 0);
    if (ret < 0)
        return ret;

    for (i = 0; i < ifile->nb_streams; i++) {
        ist   = input_streams[ifile->ist_index + i];
        avctx = ist->dec_ctx;

        /* duration is the length of the last frame in a stream
         * when audio stream is present we don't care about
         * last video frame length because it's not defined exactly */
        if (avctx->codec_type == AVMEDIA_TYPE_AUDIO && ist->nb_samples)
            has_audio = 1;
    }

    for (i = 0; i < ifile->nb_streams; i++) {
        ist   = input_streams[ifile->ist_index + i];
        avctx = ist->dec_ctx;

        if (has_audio) {
            if (avctx->codec_type == AVMEDIA_TYPE_AUDIO && ist->nb_samples) {
                AVRational sample_rate = {1, avctx->sample_rate};

                duration = av_rescale_q(ist->nb_samples, sample_rate, ist->st->time_base);
            } else {
                continue;
            }
        } else {
            if (ist->framerate.num) {
                duration = av_rescale_q(1, av_inv_q(ist->framerate), ist->st->time_base);
            } else if (ist->st->avg_frame_rate.num) {
                duration = av_rescale_q(1, av_inv_q(ist->st->avg_frame_rate), ist->st->time_base);
            } else {
                duration = 1;
            }
        }
        if (!ifile->duration)
            ifile->time_base = ist->st->time_base;
        /* the total duration of the stream, max_pts - min_pts is
         * the duration of the stream without the last frame */
        duration += ist->max_pts - ist->min_pts;
        ifile->time_base = duration_max(duration, &ifile->duration, ist->st->time_base,
                                        ifile->time_base);
    }

    if (ifile->loop > 0)
        ifile->loop--;

    return ret;
}

/*
 * Return
 * - 0 -- one packet was read and processed
 * - AVERROR(EAGAIN) -- no packets were available for selected file,
 *   this function should be called again
 * - AVERROR_EOF -- this function should not be called again
 */
static int process_input(int file_index)
{
    InputFile *ifile = input_files[file_index];
    AVFormatContext *is;
    InputStream *ist;
    AVPacket pkt;
    int ret, thread_ret, i, j;
    int64_t duration;
    int64_t pkt_dts;

    is  = ifile->ctx;
    ret = get_input_packet(ifile, &pkt);

    if (ret == AVERROR(EAGAIN)) {
        ifile->eagain = 1;
        return ret;
    }
    if (ret < 0 && ifile->loop) {
        AVCodecContext *avctx;
        for (i = 0; i < ifile->nb_streams; i++) {
            ist = input_streams[ifile->ist_index + i];
            avctx = ist->dec_ctx;
            if (ist->decoding_needed) {
                ret = process_input_packet(ist, NULL, 1);
                if (ret>0)
                    return 0;
                avcodec_flush_buffers(avctx);
            }
        }
#if HAVE_THREADS
        free_input_thread(file_index);
#endif
        ret = seek_to_start(ifile, is);
#if HAVE_THREADS
        thread_ret = init_input_thread(file_index);
        if (thread_ret < 0)
            return thread_ret;
#endif
        if (ret < 0)
            av_log(NULL, AV_LOG_WARNING, "Seek to start failed.\n");
        else
            ret = get_input_packet(ifile, &pkt);
        if (ret == AVERROR(EAGAIN)) {
            ifile->eagain = 1;
            return ret;
        }
    }
    if (ret < 0) {
        if (ret != AVERROR_EOF) {
            print_error(is->url, ret);
            if (exit_on_error)
                exit_program(1);
        }

        for (i = 0; i < ifile->nb_streams; i++) {
            ist = input_streams[ifile->ist_index + i];
            if (ist->decoding_needed) {
                ret = process_input_packet(ist, NULL, 0);
                if (ret>0)
                    return 0;
            }

            /* mark all outputs that don't go through lavfi as finished */
            for (j = 0; j < nb_output_streams; j++) {
                OutputStream *ost = output_streams[j];

                if (ost->source_index == ifile->ist_index + i &&
                    (ost->stream_copy || ost->enc->type == AVMEDIA_TYPE_SUBTITLE))
                    finish_output_stream(ost);
            }
        }

        ifile->eof_reached = 1;
        return AVERROR(EAGAIN);
    }

    reset_eagain();

    if (do_pkt_dump) {
        av_pkt_dump_log2(NULL, AV_LOG_INFO, &pkt, do_hex_dump,
                         is->streams[pkt.stream_index]);
    }
    /* the following test is needed in case new streams appear
       dynamically in stream : we ignore them */
    if (pkt.stream_index >= ifile->nb_streams) {
        report_new_stream(file_index, &pkt);
        goto discard_packet;
    }

    ist = input_streams[ifile->ist_index + pkt.stream_index];

    ist->data_size += pkt.size;
    ist->nb_packets++;

    if (ist->discard)
        goto discard_packet;

    if (pkt.flags & AV_PKT_FLAG_CORRUPT) {
        av_log(NULL, exit_on_error ? AV_LOG_FATAL : AV_LOG_WARNING,
               "%s: corrupt input packet in stream %d\n", is->url, pkt.stream_index);
        if (exit_on_error)
            exit_program(1);
    }

    if (debug_ts) {
        av_log(NULL, AV_LOG_INFO, "demuxer -> ist_index:%d type:%s "
               "next_dts:%s next_dts_time:%s next_pts:%s next_pts_time:%s pkt_pts:%s pkt_pts_time:%s pkt_dts:%s pkt_dts_time:%s off:%s off_time:%s\n",
               ifile->ist_index + pkt.stream_index, av_get_media_type_string(ist->dec_ctx->codec_type),
               av_ts2str(ist->next_dts), av_ts2timestr(ist->next_dts, &AV_TIME_BASE_Q),
               av_ts2str(ist->next_pts), av_ts2timestr(ist->next_pts, &AV_TIME_BASE_Q),
               av_ts2str(pkt.pts), av_ts2timestr(pkt.pts, &ist->st->time_base),
               av_ts2str(pkt.dts), av_ts2timestr(pkt.dts, &ist->st->time_base),
               av_ts2str(input_files[ist->file_index]->ts_offset),
               av_ts2timestr(input_files[ist->file_index]->ts_offset, &AV_TIME_BASE_Q));
    }

    if(!ist->wrap_correction_done && is->start_time != AV_NOPTS_VALUE && ist->st->pts_wrap_bits < 64){
        int64_t stime, stime2;
        // Correcting starttime based on the enabled streams
        // FIXME this ideally should be done before the first use of starttime but we do not know which are the enabled streams at that point.
        //       so we instead do it here as part of discontinuity handling
        if (   ist->next_dts == AV_NOPTS_VALUE
            && ifile->ts_offset == -is->start_time
            && (is->iformat->flags & AVFMT_TS_DISCONT)) {
            int64_t new_start_time = INT64_MAX;
            for (i=0; i<is->nb_streams; i++) {
                AVStream *st = is->streams[i];
                if(st->discard == AVDISCARD_ALL || st->start_time == AV_NOPTS_VALUE)
                    continue;
                new_start_time = FFMIN(new_start_time, av_rescale_q(st->start_time, st->time_base, AV_TIME_BASE_Q));
            }
            if (new_start_time > is->start_time) {
                av_log(is, AV_LOG_VERBOSE, "Correcting start time by %"PRId64"\n", new_start_time - is->start_time);
                ifile->ts_offset = -new_start_time;
            }
        }

        stime = av_rescale_q(is->start_time, AV_TIME_BASE_Q, ist->st->time_base);
        stime2= stime + (1ULL<<ist->st->pts_wrap_bits);
        ist->wrap_correction_done = 1;

        if(stime2 > stime && pkt.dts != AV_NOPTS_VALUE && pkt.dts > stime + (1LL<<(ist->st->pts_wrap_bits-1))) {
            pkt.dts -= 1ULL<<ist->st->pts_wrap_bits;
            ist->wrap_correction_done = 0;
        }
        if(stime2 > stime && pkt.pts != AV_NOPTS_VALUE && pkt.pts > stime + (1LL<<(ist->st->pts_wrap_bits-1))) {
            pkt.pts -= 1ULL<<ist->st->pts_wrap_bits;
            ist->wrap_correction_done = 0;
        }
    }

    /* add the stream-global side data to the first packet */
    if (ist->nb_packets == 1) {
        for (i = 0; i < ist->st->nb_side_data; i++) {
            AVPacketSideData *src_sd = &ist->st->side_data[i];
            uint8_t *dst_data;

            if (src_sd->type == AV_PKT_DATA_DISPLAYMATRIX)
                continue;

            if (av_packet_get_side_data(&pkt, src_sd->type, NULL))
                continue;

            dst_data = av_packet_new_side_data(&pkt, src_sd->type, src_sd->size);
            if (!dst_data)
                exit_program(1);

            memcpy(dst_data, src_sd->data, src_sd->size);
        }
    }

    if (pkt.dts != AV_NOPTS_VALUE)
        pkt.dts += av_rescale_q(ifile->ts_offset, AV_TIME_BASE_Q, ist->st->time_base);
    if (pkt.pts != AV_NOPTS_VALUE)
        pkt.pts += av_rescale_q(ifile->ts_offset, AV_TIME_BASE_Q, ist->st->time_base);

    if (pkt.pts != AV_NOPTS_VALUE)
        pkt.pts *= ist->ts_scale;
    if (pkt.dts != AV_NOPTS_VALUE)
        pkt.dts *= ist->ts_scale;

    pkt_dts = av_rescale_q_rnd(pkt.dts, ist->st->time_base, AV_TIME_BASE_Q, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
    if ((ist->dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO ||
         ist->dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) &&
        pkt_dts != AV_NOPTS_VALUE && ist->next_dts == AV_NOPTS_VALUE && !copy_ts
        && (is->iformat->flags & AVFMT_TS_DISCONT) && ifile->last_ts != AV_NOPTS_VALUE) {
        int64_t delta   = pkt_dts - ifile->last_ts;
        if (delta < -1LL*dts_delta_threshold*AV_TIME_BASE ||
            delta >  1LL*dts_delta_threshold*AV_TIME_BASE){
            ifile->ts_offset -= delta;
            av_log(NULL, AV_LOG_DEBUG,
                   "Inter stream timestamp discontinuity %"PRId64", new offset= %"PRId64"\n",
                   delta, ifile->ts_offset);
            pkt.dts -= av_rescale_q(delta, AV_TIME_BASE_Q, ist->st->time_base);
            if (pkt.pts != AV_NOPTS_VALUE)
                pkt.pts -= av_rescale_q(delta, AV_TIME_BASE_Q, ist->st->time_base);
        }
    }

    duration = av_rescale_q(ifile->duration, ifile->time_base, ist->st->time_base);
    if (pkt.pts != AV_NOPTS_VALUE) {
        pkt.pts += duration;
        ist->max_pts = FFMAX(pkt.pts, ist->max_pts);
        ist->min_pts = FFMIN(pkt.pts, ist->min_pts);
    }

    if (pkt.dts != AV_NOPTS_VALUE)
        pkt.dts += duration;

    pkt_dts = av_rescale_q_rnd(pkt.dts, ist->st->time_base, AV_TIME_BASE_Q, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
    if ((ist->dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO ||
         ist->dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) &&
         pkt_dts != AV_NOPTS_VALUE && ist->next_dts != AV_NOPTS_VALUE &&
        !copy_ts) {
        int64_t delta   = pkt_dts - ist->next_dts;
        if (is->iformat->flags & AVFMT_TS_DISCONT) {
            if (delta < -1LL*dts_delta_threshold*AV_TIME_BASE ||
                delta >  1LL*dts_delta_threshold*AV_TIME_BASE ||
                pkt_dts + AV_TIME_BASE/10 < FFMAX(ist->pts, ist->dts)) {
                ifile->ts_offset -= delta;
                av_log(NULL, AV_LOG_DEBUG,
                       "timestamp discontinuity for stream #%d:%d "
                       "(id=%d, type=%s): %"PRId64", new offset= %"PRId64"\n",
                       ist->file_index, ist->st->index, ist->st->id,
                       av_get_media_type_string(ist->dec_ctx->codec_type),
                       delta, ifile->ts_offset);
                pkt.dts -= av_rescale_q(delta, AV_TIME_BASE_Q, ist->st->time_base);
                if (pkt.pts != AV_NOPTS_VALUE)
                    pkt.pts -= av_rescale_q(delta, AV_TIME_BASE_Q, ist->st->time_base);
            }
        } else {
            if ( delta < -1LL*dts_error_threshold*AV_TIME_BASE ||
                 delta >  1LL*dts_error_threshold*AV_TIME_BASE) {
                av_log(NULL, AV_LOG_WARNING, "DTS %"PRId64", next:%"PRId64" st:%d invalid dropping\n", pkt.dts, ist->next_dts, pkt.stream_index);
                pkt.dts = AV_NOPTS_VALUE;
            }
            if (pkt.pts != AV_NOPTS_VALUE){
                int64_t pkt_pts = av_rescale_q(pkt.pts, ist->st->time_base, AV_TIME_BASE_Q);
                delta   = pkt_pts - ist->next_dts;
                if ( delta < -1LL*dts_error_threshold*AV_TIME_BASE ||
                     delta >  1LL*dts_error_threshold*AV_TIME_BASE) {
                    av_log(NULL, AV_LOG_WARNING, "PTS %"PRId64", next:%"PRId64" invalid dropping st:%d\n", pkt.pts, ist->next_dts, pkt.stream_index);
                    pkt.pts = AV_NOPTS_VALUE;
                }
            }
        }
    }

    if (pkt.dts != AV_NOPTS_VALUE)
        ifile->last_ts = av_rescale_q(pkt.dts, ist->st->time_base, AV_TIME_BASE_Q);

    if (debug_ts) {
        av_log(NULL, AV_LOG_INFO, "demuxer+ffmpeg -> ist_index:%d type:%s pkt_pts:%s pkt_pts_time:%s pkt_dts:%s pkt_dts_time:%s off:%s off_time:%s\n",
               ifile->ist_index + pkt.stream_index, av_get_media_type_string(ist->dec_ctx->codec_type),
               av_ts2str(pkt.pts), av_ts2timestr(pkt.pts, &ist->st->time_base),
               av_ts2str(pkt.dts), av_ts2timestr(pkt.dts, &ist->st->time_base),
               av_ts2str(input_files[ist->file_index]->ts_offset),
               av_ts2timestr(input_files[ist->file_index]->ts_offset, &AV_TIME_BASE_Q));
    }

    sub2video_heartbeat(ist, pkt.pts);

    process_input_packet(ist, &pkt, 0);

discard_packet:
    av_packet_unref(&pkt);

    return 0;
}

/**
 * Perform a step of transcoding for the specified filter graph.
 *
 * @param[in]  graph     filter graph to consider
 * @param[out] best_ist  input stream where a frame would allow to continue
 * @return  0 for success, <0 for error
 */
static int transcode_from_filter(FilterGraph *graph, InputStream **best_ist)
{
    int i, ret;
    int nb_requests, nb_requests_max = 0;
    InputFilter *ifilter;
    InputStream *ist;

    *best_ist = NULL;
    ret = avfilter_graph_request_oldest(graph->graph);
    if (ret >= 0)
        return reap_filters(0);

    if (ret == AVERROR_EOF) {
        ret = reap_filters(1);
        for (i = 0; i < graph->nb_outputs; i++)
            close_output_stream(graph->outputs[i]->ost);
        return ret;
    }
    if (ret != AVERROR(EAGAIN))
        return ret;

    for (i = 0; i < graph->nb_inputs; i++) {
        ifilter = graph->inputs[i];
        ist = ifilter->ist;
        if (input_files[ist->file_index]->eagain ||
            input_files[ist->file_index]->eof_reached)
            continue;
        nb_requests = av_buffersrc_get_nb_failed_requests(ifilter->filter);
        if (nb_requests > nb_requests_max) {
            nb_requests_max = nb_requests;
            *best_ist = ist;
        }
    }

    if (!*best_ist)
        for (i = 0; i < graph->nb_outputs; i++)
            graph->outputs[i]->ost->unavailable = 1;

    return 0;
}

/**
 * Run a single step of transcoding.
 *
 * @return  0 for success, <0 for error
 */
static int transcode_step(void)
{
    OutputStream *ost;
    InputStream  *ist = NULL;
    int ret;

    ost = choose_output();
    if (!ost) {
        if (got_eagain()) {
            reset_eagain();
            av_usleep(10000);
            return 0;
        }
        av_log(NULL, AV_LOG_VERBOSE, "No more inputs to read from, finishing.\n");
        return AVERROR_EOF;
    }

    if (ost->filter && !ost->filter->graph->graph) {
        if (ifilter_has_all_input_formats(ost->filter->graph)) {
            ret = configure_filtergraph(ost->filter->graph);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error reinitializing filters!\n");
                return ret;
            }
        }
    }

    if (ost->filter && ost->filter->graph->graph) {
        if (!ost->initialized) {
            char error[1024] = {0};
            ret = init_output_stream(ost, error, sizeof(error));
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error initializing output stream %d:%d -- %s\n",
                       ost->file_index, ost->index, error);
                exit_program(1);
            }
        }
        if ((ret = transcode_from_filter(ost->filter->graph, &ist)) < 0)
            return ret;
        if (!ist)
            return 0;
    } else if (ost->filter) {
        int i;
        for (i = 0; i < ost->filter->graph->nb_inputs; i++) {
            InputFilter *ifilter = ost->filter->graph->inputs[i];
            if (!ifilter->ist->got_output && !input_files[ifilter->ist->file_index]->eof_reached) {
                ist = ifilter->ist;
                break;
            }
        }
        if (!ist) {
            ost->inputs_done = 1;
            return 0;
        }
    } else {
        av_assert0(ost->source_index >= 0);
        ist = input_streams[ost->source_index];
    }

    ret = process_input(ist->file_index);
    if (ret == AVERROR(EAGAIN)) {
        if (input_files[ist->file_index]->eagain)
            ost->unavailable = 1;
        return 0;
    }

    if (ret < 0)
        return ret == AVERROR_EOF ? 0 : ret;

    return reap_filters(0);
}

/*
 * The following code is the main loop of the file converter
 */
static int transcode(void)
{
    int ret, i;
    AVFormatContext *os;
    OutputStream *ost;
    InputStream *ist;
    int64_t timer_start;
    int64_t total_packets_written = 0;

    ret = transcode_init();
    if (ret < 0)
        goto fail;

    if (stdin_interaction) {
        av_log(NULL, AV_LOG_INFO, "Press [q] to stop, [?] for help\n");
    }

    timer_start = av_gettime_relative();

#if HAVE_THREADS
    if ((ret = init_input_threads()) < 0)
        goto fail;
#endif

    while (!received_sigterm) {
        int64_t cur_time= av_gettime_relative();

        /* if 'q' pressed, exits */
        if (stdin_interaction)
            if (check_keyboard_interaction(cur_time) < 0)
                break;

        /* check if there's any stream where output is still needed */
        if (!need_output()) {
            av_log(NULL, AV_LOG_VERBOSE, "No more output streams to write to, finishing.\n");
            break;
        }

        ret = transcode_step();
        if (ret < 0 && ret != AVERROR_EOF) {
            av_log(NULL, AV_LOG_ERROR, "Error while filtering: %s\n", av_err2str(ret));
            break;
        }

        /* dump report by using the output first video and audio streams */
        print_report(0, timer_start, cur_time);
    }
#if HAVE_THREADS
    free_input_threads();
#endif

    /* at the end of stream, we must flush the decoder buffers */
    for (i = 0; i < nb_input_streams; i++) {
        ist = input_streams[i];
        if (!input_files[ist->file_index]->eof_reached) {
            process_input_packet(ist, NULL, 0);
        }
    }
    flush_encoders();

    term_exit();

    /* write the trailer if needed and close file */
    for (i = 0; i < nb_output_files; i++) {
        os = output_files[i]->ctx;
        if (!output_files[i]->header_written) {
            av_log(NULL, AV_LOG_ERROR,
                   "Nothing was written into output file %d (%s), because "
                   "at least one of its streams received no packets.\n",
                   i, os->url);
            continue;
        }
        if ((ret = av_write_trailer(os)) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error writing trailer of %s: %s\n", os->url, av_err2str(ret));
            if (exit_on_error)
                exit_program(1);
        }
    }

    /* dump report by using the first video and audio streams */
    print_report(1, timer_start, av_gettime_relative());

    /* close each encoder */
    for (i = 0; i < nb_output_streams; i++) {
        ost = output_streams[i];
        if (ost->encoding_needed) {
            av_freep(&ost->enc_ctx->stats_in);
        }
        total_packets_written += ost->packets_written;
    }

    if (!total_packets_written && (abort_on_flags & ABORT_ON_FLAG_EMPTY_OUTPUT)) {
        av_log(NULL, AV_LOG_FATAL, "Empty output\n");
        exit_program(1);
    }

    /* close each decoder */
    for (i = 0; i < nb_input_streams; i++) {
        ist = input_streams[i];
        if (ist->decoding_needed) {
            avcodec_close(ist->dec_ctx);
            if (ist->hwaccel_uninit)
                ist->hwaccel_uninit(ist->dec_ctx);
        }
    }

    av_buffer_unref(&hw_device_ctx);
    hw_device_free_all();

    /* finished ! */
    ret = 0;

 fail:
#if HAVE_THREADS
    free_input_threads();
#endif

    if (output_streams) {
        for (i = 0; i < nb_output_streams; i++) {
            ost = output_streams[i];
            if (ost) {
                if (ost->logfile) {
                    if (fclose(ost->logfile))
                        av_log(NULL, AV_LOG_ERROR,
                               "Error closing logfile, loss of information possible: %s\n",
                               av_err2str(AVERROR(errno)));
                    ost->logfile = NULL;
                }
                av_freep(&ost->forced_kf_pts);
                av_freep(&ost->apad);
                av_freep(&ost->disposition);
                av_dict_free(&ost->encoder_opts);
                av_dict_free(&ost->sws_dict);
                av_dict_free(&ost->swr_opts);
                av_dict_free(&ost->resample_opts);
            }
        }
    }
    return ret;
}

static BenchmarkTimeStamps get_benchmark_time_stamps(void)
{
    BenchmarkTimeStamps time_stamps = { av_gettime_relative() };
#if HAVE_GETRUSAGE
    struct rusage rusage;

    getrusage(RUSAGE_SELF, &rusage);
    time_stamps.user_usec =
        (rusage.ru_utime.tv_sec * 1000000LL) + rusage.ru_utime.tv_usec;
    time_stamps.sys_usec =
        (rusage.ru_stime.tv_sec * 1000000LL) + rusage.ru_stime.tv_usec;
#elif HAVE_GETPROCESSTIMES
    HANDLE proc;
    FILETIME c, e, k, u;
    proc = GetCurrentProcess();
    GetProcessTimes(proc, &c, &e, &k, &u);
    time_stamps.user_usec =
        ((int64_t)u.dwHighDateTime << 32 | u.dwLowDateTime) / 10;
    time_stamps.sys_usec =
        ((int64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) / 10;
#else
    time_stamps.user_usec = time_stamps.sys_usec = 0;
#endif
    return time_stamps;
}

static int64_t getmaxrss(void)
{
#if HAVE_GETRUSAGE && HAVE_STRUCT_RUSAGE_RU_MAXRSS
    struct rusage rusage;
    getrusage(RUSAGE_SELF, &rusage);
    return (int64_t)rusage.ru_maxrss * 1024;
#elif HAVE_GETPROCESSMEMORYINFO
    HANDLE proc;
    PROCESS_MEMORY_COUNTERS memcounters;
    proc = GetCurrentProcess();
    memcounters.cb = sizeof(memcounters);
    GetProcessMemoryInfo(proc, &memcounters, sizeof(memcounters));
    return memcounters.PeakPagefileUsage;
#else
    return 0;
#endif
}

static void log_callback_null(void *ptr, int level, const char *fmt, va_list vl)
{
}

//#include <x264.h>
#include <malloc.h>
#define MAX_MATRIX_SIZE 63
#define FHD_BUFFER_SIZE 0x9867000 //1920*1088*51*3/2 //0x956A000 //1920*1088*50*3/2

#define INBUF_SIZE 1024*1024*300
#define DECODE_FRAME_NUM_PER_GOP 50
#define MIN_NUM_OF_PER_GOP 300
#define FILTERED_FRAME_NUM_PER_GOP 10

float pixel_sharpness_val = 0.0;

static long long saved_data_size = 0;
static long long saved_size = 0;
int enc_pkt_size[DECODE_FRAME_NUM_PER_GOP];

static long long saved_data_size_filtered = 0;
int enc_pkt_size_filtered[DECODE_FRAME_NUM_PER_GOP];

typedef struct MemInfo {
	uint8_t *pVideoBuffer;
	uint8_t *pVideoBufferCrf5;

	uint8_t *pVideoBuffer1;
	uint8_t *pEncodeVideoBuffer;
	uint8_t *pDecodeVideoBuffer;

	uint8_t *pVideoBuffer2;
	uint8_t *pEncodeVideoBuffer2;
	uint8_t *pDecodeVideoBuffer2;
}MemInfo;
#if 0
typedef struct X264Context {
    AVClass        *class;
    x264_param_t    params;
    x264_t         *enc;
    x264_picture_t  pic;
    uint8_t        *sei;
    int             sei_size;
    char *preset;
    char *tune;
    char *profile;
    char *level;
    int fastfirstpass;
    char *wpredp;
    char *x264opts;
    float crf;
    float crf_max;
    int cqp;
    int aq_mode;
    float aq_strength;
    char *psy_rd;
    int psy;
    int rc_lookahead;
    int weightp;
    int weightb;
    int ssim;
    int intra_refresh;
    int bluray_compat;
    int b_bias;
    int b_pyramid;
    int mixed_refs;
    int dct8x8;
    int fast_pskip;
    int aud;
    int mbtree;
    char *deblock;
    float cplxblur;
    char *partitions;
    int direct_pred;
    int slice_max_size;
    char *stats;
    int nal_hrd;
    int avcintra_class;
    int motion_est;
    int forced_idr;
    int coder;
    int a53_cc;
    int b_frame_strategy;
    int chroma_offset;
    int scenechange_threshold;
    int noise_reduction;

    char *x264_params;

    int nb_reordered_opaque, next_reordered_opaque;
    int64_t *reordered_opaque;

    /**
     * If the encoder does not support ROI then warn the first time we
     * encounter a frame with ROI side data.
     */
    int roi_warned;
} X264Context;
#endif
typedef struct InputParams {
    char *src_filename;
    char *video_dst_filename;
    char *audio_dst_filename;
    FILE *src_file;
    FILE *video_dst_file;
    FILE *audio_dst_file;
}InputParams;

typedef struct InputStreamInfo {
    AVPacket                *p_pkt;
    AVFrame               *p_frame;
    AVFormatContext     *p_fmt_ctx;
    AVStream            *p_video_stream;
    AVStream            *p_audio_stream;
    AVCodec             *p_video_codec;
    AVCodec             *p_audio_codec;
    AVCodecContext      *p_video_codecctx;
    AVCodecContext      *p_audio_codecctx;
    AVCodecParameters   *p_video_codec_par;
    AVCodecParameters   *p_audio_codec_par;
    int                 width, height;
    int video_stream_idx, audio_stream_idx;
}InputStreamInfo;

typedef struct DecodeInfo {
    int width, height;
    uint8_t *video_dst_data[4];
    int video_dst_linesize[4];
    int video_dst_bufsize;
    enum AVPixelFormat pix_fmt;
	long long dec_frame_num;
} DecodeInfo;

typedef struct EncodeInfo {
    AVCodec *codec;
    AVCodecContext *codecCtx;
    AVFrame *frame;
    AVPacket *p_pkt;
    AVPacket pkt;
} EncodeInfo;

struct data {
	char *format;
	int width;
	int height;
	size_t offset;
	FILE *ref_rfile;
	FILE *dis_rfile;
	int num_frames;
};

struct newData {
	char *format;
	int width;
	int height;
	size_t offset;
	uint8_t *ref;
	uint8_t *dis;
	int num_frames;
	int stage;
};

typedef struct DecEncH264FmtInfo {
	AVCodec *codec;
	AVCodecContext *codecCtx;
	AVCodecParserContext *pCodecParserCtx;
	AVFrame *frame;
	AVPacket pkt;
	uint8_t *inbuf;
	uint8_t *pDataPtr;
	size_t uDataSize;
	FILE *outputfp;
}DecEncH264FmtInfo;

typedef struct UnsharpFilterInfo
{
	int width, height;
	char filter_descr[100];
	AVFrame *frame_in;
	AVFrame *frame_out;
	unsigned char *frame_buffer_in;
	unsigned char *frame_buffer_out;
	AVFilterContext *buffersink_ctx;
	AVFilterContext *buffersrc_ctx;
	AVFilterGraph *filter_graph;
	AVFilterInOut *outputs;
	AVFilterInOut *inputs;
	int filtered_frame_num;
}UnsharpFilterInfo;

typedef struct EncodeParams {
	int target_vmaf_score[1000];
	int crf_per_gop[1000];
	int unsharp_per_gop[1000];
	int aq_strength_per_gop[1000];
}EncodeParams;

static int read_image_new_b(uint8_t *data, float *buf, float off, int width, int height, int stride, int ref_flag, int stage)
{
	char *byte_ptr = (char *)buf;
	unsigned char *tmp_buf = 0;
	int i, j;
	int ret = 1;
	static int first_flag   = 1;
	static int end_of_frame = 0;
	static int reserved_ref = 0;
	static int reserved_dis = 0;

	if (end_of_frame == 1) {
		first_flag   = 1;
		end_of_frame = 0;
		reserved_ref = 0;
		reserved_dis = 0;
		return 2;
	}
	if (first_flag) {
		first_flag = 0;
		if (stage == 1) {
			reserved_ref = width * height * (DECODE_FRAME_NUM_PER_GOP - 5) * 3 / 2;
			reserved_dis = width * height * (DECODE_FRAME_NUM_PER_GOP - 5) * 3 / 2;
		} else if (stage == 2) {
			reserved_ref = width * height * (FILTERED_FRAME_NUM_PER_GOP - 5) * 3 / 2;
			reserved_dis = width * height * (FILTERED_FRAME_NUM_PER_GOP - 5) * 3 / 2;
		}
	}

	if (width <= 0 || height <= 0)
		goto fail_or_end;
	
	if (!(tmp_buf = malloc(width)))
		goto fail_or_end;

	for (i = 0; i < height; ++i) {
		float *row_ptr = (float *)byte_ptr;

		if (ref_flag) {
			if (stage == 1)
				memcpy(tmp_buf, data + (DECODE_FRAME_NUM_PER_GOP - 5) * width * height * 3 / 2 - reserved_ref + i * width, width);
			else if (stage == 2)
				memcpy(tmp_buf, data + (FILTERED_FRAME_NUM_PER_GOP - 5) * width * height * 3 / 2 - reserved_ref + i * width, width);
		} else {
			if (stage == 1)
				memcpy(tmp_buf, data + (DECODE_FRAME_NUM_PER_GOP - 5) * width * height * 3 / 2 - reserved_dis + i * width, width);
			else if (stage == 2)
				memcpy(tmp_buf, data + (FILTERED_FRAME_NUM_PER_GOP - 5)* width * height * 3 / 2 - reserved_dis + i * width, width);
		}
		for (j = 0; j < width; j++)
			row_ptr[j] = tmp_buf[j] + off;

		byte_ptr += stride;
	}

	free(tmp_buf);

	if (ref_flag)
		reserved_ref -= width * height * 3 / 2;
	else
		reserved_dis -= width * height * 3 / 2;

	if (reserved_ref == 0 || reserved_dis == 0)
	{
		first_flag   = 1;
		end_of_frame = 1;
		return 0;
	}

	ret = 0;

fail_or_end:
	//free(tmp_buf);
	return 0;
}

/**
 * Note: stride is in terms of bytes
 */
static int read_image_b(FILE *rfile, float *buf, float off, int width, int height, int stride)
{
	char *byte_ptr = (char *)buf;
	unsigned char *tmp_buf = 0;
	int i, j;
	int ret = 1;

	if (width <= 0 || height <= 0)
		goto fail_or_end;

	if (!(tmp_buf = malloc(width)))
		goto fail_or_end;

	for (i = 0; i < height; i++) {
		float *row_ptr = (float *)byte_ptr;

		if (fread(tmp_buf, 1, width, rfile) != (size_t)width)
			goto fail_or_end;
		for (j = 0; j < width; j++)
			row_ptr[j] = tmp_buf[j] + off;

		byte_ptr += stride;
	}

	ret = 0;

fail_or_end:
	free(tmp_buf);
	return ret;
}

static int completed_frames = 0;

static int read_frame_new(float *ref_data, float *dis_data, float *temp_data, int stride_byte, void *s)
{
	struct newData *user_data = (struct newData *)s;
	char *fmt = user_data->format;
	int w = user_data->width;
	int h = user_data->height;
	int ret_ref,ret_dis;
	uint8_t *frame_data = (uint8_t *)dis_data;

	//read ref y
	if (!strcmp(fmt, "yuv420p")) {
		ret_ref = read_image_new_b(user_data->ref, ref_data, 0, w, h, stride_byte, 1, user_data->stage);
	} else {
		fprintf(stderr, "Eagle: unknown format %s.\n", fmt);
		return 1;		
	}

	//read dis y
	if (!strcmp(fmt, "yuv420p")) {
		ret_dis = read_image_new_b(user_data->dis, dis_data, 0, w, h, stride_byte, 0, user_data->stage);
	} else {
		fprintf(stderr, "Eagle: unknown format %s.\n", fmt);
		return 1;
	}
	if (ret_ref == 2 || ret_dis == 2)
		return 2;

	//fprintf(stderr, "Frame: %d/%d\r", completed_frames++, user_data->num_frames);

	return 0;
}

static int read_frame(float *ref_data, float *dis_data, float *temp_data, int stride_byte, void *s)
{
    struct data *user_data = (struct data *)s;
    char *fmt = user_data->format;
    int w = user_data->width;
    int h = user_data->height;
    int ret;
	uint8_t *frame_data = (uint8_t *)dis_data;

	//read ref y
	if (!strcmp(fmt, "yuv420p") || !strcmp(fmt, "yuv422p") || !strcmp(fmt, "yuv444p")) {
		ret = read_image_b(user_data->ref_rfile, ref_data, 0, w, h, stride_byte);
	} else {
		fprintf(stderr, "Eagle: unknown format %s.\n", fmt);
		return 1;
	}
	if (ret) {
		if (feof(user_data->ref_rfile))
			ret = 2;
		return ret;
	}

	//read dis y
	if (!strcmp(fmt, "yuv420p")) {
		ret = read_image_b(user_data->dis_rfile, dis_data, 0, w, h, stride_byte);
	} else {
		fprintf(stderr, "Eagle: unknown format %s.\n", fmt);
		return 1;
	}
	if (ret) {
		if (feof(user_data->dis_rfile))
			ret = 2;
		return ret;
	}

	//ref skip u and v
	if (!strcmp(fmt, "yuv420p")) {
		if (fread(temp_data, 1, user_data->offset, user_data->ref_rfile) != (size_t)user_data->offset) {
			fprintf(stderr, "Eagle: ref fread u an v failed.\n");
			goto fail_or_end;
		}
	} else {
		fprintf(stderr, "Eagle: unknown format %s.\n", fmt);
		goto fail_or_end;
	}

	//dis skip u and v
	if (!strcmp(fmt, "yuv420p")) {
		if (fread(temp_data, 1, user_data->offset, user_data->dis_rfile) != (size_t)user_data->offset) {
			fprintf(stderr, "Eagle: dis fread u and v failed.\n");
			goto fail_or_end;
		}
	} else {
		fprintf(stderr, "Eagle: Frame %d/%d\r", completed_frames++, user_data->num_frames);
	}

fail_or_end:
	return ret;
}

static void fill_yuv_image_one(uint8_t *data[4], int linesize[4],
								int width, int height, int frame_index, MemInfo *pmeminfo)
{
	int x, y;
	int frame_size = width * height * 3 / 2;

	for (int plane = 0; plane < 3; plane++) {
		int frame_height = plane == 0 ? height : height >> 1;
		int frame_width  = plane == 0 ? width  : width  >> 1;
		int plane_size   = frame_height * frame_width;
		int plane_stride = linesize[plane];

		if (frame_width == plane_stride) {
			if (plane == 0) {
				memcpy(data[plane], pmeminfo->pVideoBuffer1+ frame_index * frame_size, plane_size);
			} else if (plane == 1) {
				memcpy(data[plane], pmeminfo->pVideoBuffer1 + frame_index * frame_size + width * height, plane_size);
			} else if (plane == 2) {
				memcpy(data[plane], pmeminfo->pVideoBuffer1 + frame_index * frame_size + width * height + (width >> 1) * (height >> 1), plane_size);
			}
		} else {
			for (int row_idx = 0; row_idx < frame_height; row_idx++) {
				memcpy(data[plane] + row_idx * plane_stride, 
					pmeminfo->pVideoBuffer1 + frame_index * plane_size + row_idx * plane_stride, frame_width);
			}
		}
	}
}


static void fill_yuv_image(uint8_t *data[4], int linesize[4],
								int width, int height, int frame_index, MemInfo *pmeminfo)
{
	int x, y;
	int frame_size = width * height * 3 / 2;

	for (int plane = 0; plane < 3; plane++) {
		int frame_height = plane == 0 ? height : height >> 1;
		int frame_width  = plane == 0 ? width  : width  >> 1;
		int plane_size   = frame_height * frame_width;
		int plane_stride = linesize[plane];

		if (frame_width == plane_stride) {
			if (plane == 0) {
				memcpy(data[plane], pmeminfo->pVideoBuffer + frame_index * frame_size, plane_size);
			} else if (plane == 1) {
				memcpy(data[plane], pmeminfo->pVideoBuffer + frame_index * frame_size + width * height, plane_size);
			} else if (plane == 2) {
				memcpy(data[plane], pmeminfo->pVideoBuffer + frame_index * frame_size + width * height + (width >> 1) * (height >> 1), plane_size);
			}
		} else {
			for (int row_idx = 0; row_idx < frame_height; row_idx++) {
				memcpy(data[plane] + row_idx * plane_stride, 
					pmeminfo->pVideoBuffer + frame_index * plane_size + row_idx * plane_stride, frame_width);
			}
		}
	}
}

static void fill_yuv_image_filtered(uint8_t *data[4], int linesize[4],
								int width, int height, int frame_index, MemInfo *pmeminfo)
{
	int x, y;
	int frame_size = width * height * 3 / 2;

	for (int plane = 0; plane < 3; plane++) {
		int frame_height = plane == 0 ? height : height >> 1;
		int frame_width  = plane == 0 ? width  : width  >> 1;
		int plane_size   = frame_height * frame_width;
		int plane_stride = linesize[plane];

		if (frame_width == plane_stride) {
			if (plane == 0) {
				memcpy(data[plane], pmeminfo->pVideoBuffer2 + frame_index * frame_size, plane_size);
			} else if (plane == 1) {
				memcpy(data[plane], pmeminfo->pVideoBuffer2 + frame_index * frame_size + width * height, plane_size);
			} else if (plane == 2) {
				memcpy(data[plane], pmeminfo->pVideoBuffer2 + frame_index * frame_size + width * height + (width >> 1) * (height >> 1), plane_size);
			}
		} else {
			for (int row_idx = 0; row_idx < frame_height; row_idx++) {
				memcpy(data[plane] + row_idx * plane_stride, 
					pmeminfo->pVideoBuffer2 + frame_index * plane_size + row_idx * plane_stride, frame_width);
			}
		}
	}
}


static int encode_frame(InputStreamInfo *p_input_stream_info, EncodeInfo *p_enc_info, float crf_val, int filtered_flag, MemInfo *pmeminfo, int first_loop)
{
	int ret = 0;
	static int enc_frame_num = 0;
	int encode_total_num = 0;//filtered_flag ? 10 : DECODE_FRAME_NUM_PER_GOP;
	int receive_frame_num = 0;
	int receive_frame_total_num = 0;
	if (first_loop)
		receive_frame_total_num = 48;
	else
		receive_frame_total_num = 49;

	if (filtered_flag)
		encode_total_num = 10;
	else if (first_loop)
		encode_total_num = DECODE_FRAME_NUM_PER_GOP - 1;
	else
		encode_total_num = DECODE_FRAME_NUM_PER_GOP;

	{
		//reconfig encoder params(crf), using the x264_encoder_reconfig
		X264Context *x4 = p_enc_info->codecCtx->priv_data;
		x4->params.rc.f_rf_constant = x4->crf = crf_val;
		x264_encoder_reconfig(x4->enc, &(x4->params));
	}

	for (int i = 0; i < encode_total_num; i++) {
		if (!filtered_flag) {
			if (first_loop) {
				fill_yuv_image_one(p_enc_info->frame->data, p_enc_info->frame->linesize,
							p_enc_info->codecCtx->width, p_enc_info->codecCtx->height, i, pmeminfo);

			} else {
				fill_yuv_image(p_enc_info->frame->data, p_enc_info->frame->linesize,
							p_enc_info->codecCtx->width, p_enc_info->codecCtx->height, i, pmeminfo);
			}
		} else {
			fill_yuv_image_filtered(p_enc_info->frame->data, p_enc_info->frame->linesize,
						p_enc_info->codecCtx->width, p_enc_info->codecCtx->height, i, pmeminfo);
		}

		p_enc_info->frame->pts = i;

		//encode the image
		ret = avcodec_send_frame(p_enc_info->codecCtx, p_enc_info->frame);
		if (ret == AVERROR_EOF) {
			fprintf(stderr, "receive AVERROR_EOF in the encode_frame part p_enc_info->frame %p\n", p_enc_info->frame);
			break;
		} else if (ret < 0) {
			fprintf(stderr, "Eagle: Error sending a frame for encoding in decode part\n");
			fprintf(stderr, "ret %x AVERROR(EAGAIN) %x AVERROR_EOF %x AVERROR(EINVAL) %x AVERROR(ENOMEM) %x\n",
							ret, AVERROR(EAGAIN), AVERROR_EOF, AVERROR(EINVAL), AVERROR(ENOMEM));
			return ret;
		}

		while (ret >= 0) {
			ret = avcodec_receive_packet(p_enc_info->codecCtx, p_enc_info->p_pkt);
			if (ret == AVERROR(EAGAIN)) {
				//fprintf(stderr, "Eagle: could not receive packet AVERROR_EOF %x AVERROR(EAGAIN) %x ret %x\n",
				//				AVERROR_EOF, AVERROR(EAGAIN), ret);
				continue;
			} else if (ret < 0) {
				fprintf(stderr, "Eagle: error during encoding\n");
				return ret;
			}

			if (!filtered_flag)
			{
				//save the h264 format data into buffer to decode it
				memcpy(pmeminfo->pEncodeVideoBuffer + saved_data_size, p_enc_info->p_pkt->data, p_enc_info->p_pkt->size);
				if (receive_frame_num < receive_frame_total_num) {
					//printf("receive_frame_num %d saved_size %d\n", receive_frame_num, saved_size);
					receive_frame_num++;
					saved_size += p_enc_info->p_pkt->size;
				}
				saved_data_size += p_enc_info->p_pkt->size;
				enc_pkt_size[enc_frame_num++] = p_enc_info->p_pkt->size;
			} else {
				memcpy(pmeminfo->pEncodeVideoBuffer2 + saved_data_size_filtered, p_enc_info->p_pkt->data, p_enc_info->p_pkt->size);
				saved_data_size_filtered += p_enc_info->p_pkt->size;
				enc_pkt_size_filtered[enc_frame_num++] = p_enc_info->p_pkt->size;
			}
			av_packet_unref(p_enc_info->p_pkt);
		}
	}

	//flush
	ret = avcodec_send_frame(p_enc_info->codecCtx, NULL);
	if (ret < 0) {
		fprintf(stderr, "Eagle: error sending a frame for encoding in flush part ret %x\n", ret);
		exit(1);
	}

	while (ret >= 0) {
		ret = avcodec_receive_packet(p_enc_info->codecCtx, p_enc_info->p_pkt);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			break;
		} else if (ret < 0) {
			fprintf(stderr, "Eagle: error during encoding\n");
			exit(1);
		}

		if (!filtered_flag)
		{
			//save the h264 format data into buffer to decode it
			memcpy(pmeminfo->pEncodeVideoBuffer + saved_data_size, p_enc_info->p_pkt->data, p_enc_info->p_pkt->size);
			if (receive_frame_num < receive_frame_total_num) {
				//printf("receive_frame_num %d saved_size %d\n", receive_frame_num, saved_size);
				receive_frame_num++;
				saved_size += p_enc_info->p_pkt->size;
			}
			saved_data_size += p_enc_info->p_pkt->size;
			enc_pkt_size[enc_frame_num++] = p_enc_info->p_pkt->size;
		} else {
			memcpy(pmeminfo->pEncodeVideoBuffer2 + saved_data_size_filtered, p_enc_info->p_pkt->data, p_enc_info->p_pkt->size);
			saved_data_size_filtered += p_enc_info->p_pkt->size;
			enc_pkt_size_filtered[enc_frame_num++] = p_enc_info->p_pkt->size;
		}
		av_packet_unref(p_enc_info->p_pkt);
	}

	enc_frame_num = 0;

	return 0;
}

static int encode_prepare(InputStreamInfo *p_input_stream_info, EncodeInfo *p_enc_info, DecodeInfo *p_dec_info, int tune_flag, int fps)
{
    int ret = 0;
    enum AVCodecID codec_id = AV_CODEC_ID_H264;

	p_enc_info->codec = avcodec_find_encoder(codec_id);
    if (!p_enc_info->codec) {
        fprintf(stderr, "Eagle: could not find the encoder\n");
        return -1;
    }

    p_enc_info->codecCtx = avcodec_alloc_context3(p_enc_info->codec);
    if (!p_enc_info->codecCtx) {
        fprintf(stderr, "Eagle: could not allocate video codec context\n");
        return -1;
    }

    p_enc_info->codecCtx->width     = p_dec_info->width;
    p_enc_info->codecCtx->height    = p_dec_info->height;
    p_enc_info->codecCtx->pix_fmt   = AV_PIX_FMT_YUV420P;
	p_enc_info->codecCtx->time_base = (AVRational){1, fps};
	p_enc_info->codecCtx->framerate = (AVRational){fps, 1};

	printf("fps %d\n", fps);

    av_opt_set(p_enc_info->codecCtx->priv_data, "profile", "high",   0);
    av_opt_set(p_enc_info->codecCtx->priv_data, "preset",  "medium", 0);
	if (tune_flag) {
    	av_opt_set(p_enc_info->codecCtx->priv_data, "tune",    "ssim",   0);
	}

    // open the encoder
    if (avcodec_open2(p_enc_info->codecCtx, p_enc_info->codec, NULL) < 0) {
        fprintf(stderr, "Eagle: Open encoder fail\n");
        return -1;
    }

    //allocate AVFrame structure
    p_enc_info->frame = av_frame_alloc();
    if (!p_enc_info->frame) {
        fprintf(stderr, "Eagle: could not allocate video frame\n");
        return -1;
    }

    p_enc_info->frame->width       = p_enc_info->codecCtx->width;
    p_enc_info->frame->height      = p_enc_info->codecCtx->height;
    p_enc_info->frame->format      = p_enc_info->codecCtx->pix_fmt;
	p_enc_info->frame->linesize[0] = p_input_stream_info->p_frame->linesize[0];
	p_enc_info->frame->linesize[1] = p_input_stream_info->p_frame->linesize[1];
	p_enc_info->frame->linesize[2] = p_input_stream_info->p_frame->linesize[2];

	//allocate AVFrame data
	ret = av_frame_get_buffer(p_enc_info->frame, 32);
	if (ret < 0) {
		fprintf(stderr, "Eagle: could not allocate the video frame data\n");
		return ret;
	}

    //init AVPacket
    av_init_packet(&p_enc_info->pkt);
    p_enc_info->pkt.data = NULL;
    p_enc_info->pkt.size = 0;

    p_enc_info->p_pkt = av_packet_alloc();
    if (!p_enc_info->p_pkt) {
        fprintf(stderr, "Eagle: could not allocate pkt\n");
        return -1;
    }

    return ret;
}

static long long get_unsharp_val(uint8_t *data, int width, int height, double amounts, int msize_x, int msize_y)
{
    int tmp1, tmp2;
    int res;
    long long sharpness = 0;
    const int amount = amounts * 65536;
    const int steps_x = msize_x >> 1;
    const int steps_y = msize_y >> 1;
    const int32_t scalebits = (steps_x + steps_y) * 2;
    const int32_t halfscale = 1 << (scalebits - 1);

    uint32_t *sc[MAX_MATRIX_SIZE - 1] = {NULL};

    for (int z = 0; z < 2 * steps_y; z++) {
        sc[z] = (uint32_t *)malloc((width + 2 * steps_x) * sizeof(uint32_t));
        memset(sc[z], 0, sizeof(sc[z][0]) * (width + 2 * steps_x));
    }

    //y from 1 not -steps_y
    //x from 1 not -steps_x
    for (int y = steps_y; y < height - 1; y++) {
        uint32_t sr[MAX_MATRIX_SIZE - 1] = {0};
        for (int x = steps_x; x < width - 1; x++) {
            tmp1 = data[y * width + x];
            for (int z = 0; z < steps_x * 2; z += 2) {
                tmp2 = sr[z + 0] + tmp1;
                sr[z + 0] = tmp1;
                tmp1 = sr[z + 1] + tmp2;
                sr[z + 1] = tmp2;
            }

            for (int z = 0; z < steps_y * 2; z += 2) {
                tmp2 = sc[z + 0][x + steps_x] + tmp1;
                sc[z + 0][x + steps_x] = tmp1;
                tmp1 = sc[z + 1][x + steps_x] + tmp2;
                sc[z + 1][x + steps_x] = tmp2;
            }

            /*res = data[y * width + x] + (((data[y * height + x] - (int32_t)((tmp1 + halfscale) >> scalebits)) * amount) >> 16);*/
            res = ((data[y * width + x] - (int32_t)((tmp1 + halfscale) >> scalebits)) * amount) >> 16;
            sharpness += res;
        }
    }

	for (int z = 0; z < 2 * steps_y; z++) {
		free(sc[z]);
	}

    return sharpness;
}

static int decode_prepare(InputStreamInfo **p_input_stream_info, DecodeInfo *p_dec_info)
{
    InputStreamInfo *ps = *p_input_stream_info;

    p_dec_info->width  = ps->p_video_codec_par->width;
    p_dec_info->height = ps->p_video_codec_par->height;
    p_dec_info->pix_fmt = ps->p_video_codec_par->format;
    p_dec_info->video_dst_bufsize = av_image_alloc(p_dec_info->video_dst_data, p_dec_info->video_dst_linesize,
                                                    p_dec_info->width, p_dec_info->height, p_dec_info->pix_fmt, 1);

    if (p_dec_info->video_dst_bufsize < 0) {
        fprintf(stderr, "Eagle could not allocate raw video buffer\n");
        return -1;
    }

    //allocate frame to store the decoded output data
    ps->p_frame = av_frame_alloc();
    if (!ps->p_frame) {
        fprintf(stderr, "Eagle: could not allocate frame\n");
        return -1;
    }

    //allocate avpkt to store the raw data to be decode
    ps->p_pkt = av_packet_alloc();
    if (!ps->p_pkt) {
        fprintf(stderr, "Eagle: coudl not allocate packet\n");
        return -1;
    }

    return 0;
}

static int open_video_codec_and_context(InputStreamInfo **pp_input_stream_info)
{
    int ret = 0;
    int stream_idx = -1;
    InputStreamInfo *ps = *pp_input_stream_info;

    //open video codec
    ret = av_find_best_stream(ps->p_fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "Eagle: could not find %s stream in input file\n",
                        av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
        return ret;
    }

    ps->video_stream_idx = ret;
    stream_idx = ps->video_stream_idx;
    ps->p_video_stream = ps->p_fmt_ctx->streams[stream_idx];

    //find decoder for the stream
    ps->p_video_codec_par = ps->p_video_stream->codecpar;
    ps->p_video_codec     = avcodec_find_decoder(ps->p_video_codec_par->codec_id);
    if (!ps->p_video_codec) {
        fprintf(stderr, "Eagle: failed to find %s codec\n", 
            av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
        return AVERROR(EINVAL);
    }

    //get the codeccontext
    ps->p_video_codecctx = (AVCodecContext *)avcodec_alloc_context3(ps->p_video_codec);
    if (!ps->p_video_codec) {
        fprintf(stderr, "Eagle: could not allocate video codec context\n");
        return AVERROR(ENOMEM);
    }

    //copy codec parameters from input stream to output codec context
    if ((ret = avcodec_parameters_to_context(ps->p_video_codecctx, ps->p_video_codec_par)) < 0) {
        fprintf(stderr, "Eagle: failed to copy %s codec parameters to decoder context\n",
                av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
        return ret;
    }

    //open video decoder
    if ((ret = avcodec_open2(ps->p_video_codecctx, ps->p_video_codec, NULL)) < 0) {
        fprintf(stderr, "Eagle: failed to pen %s codec\n", 
            av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
        return ret;
    }

    return ret;    
}

static int open_audio_codec_and_context(InputStreamInfo **pp_input_stream_info)
{
    return 0;
}

static int open_codecs_and_contexts(InputStreamInfo **pp_input_stream_info) {
    int ret = 0;

    ret = open_video_codec_and_context(pp_input_stream_info);
    if (ret != 0) {
        fprintf(stderr, "Eagle: Open video Codec and Context fail\n");
        return ret;
    }

    ret = open_audio_codec_and_context(pp_input_stream_info);
    if (ret != 0) {
        fprintf(stderr, "Eagle: Open audio Codec and Context fail\n");
        return ret;
    }

    return ret;
}


static int get_input_fmt(InputStreamInfo **pp_input_stream_info, char *filename)
{
    int ret = 0;
    InputStreamInfo *p_input_stream_info = *pp_input_stream_info;

    //Open input file, and allocate format context
    if ((ret = avformat_open_input(&(p_input_stream_info->p_fmt_ctx), filename, NULL, NULL) < 0)) {
        fprintf(stderr, "Eagle:could not open source file %s\n", filename);
        return ret;
    }


    //retrive stream information
    if ((ret = avformat_find_stream_info(p_input_stream_info->p_fmt_ctx, NULL) < 0)) {
        fprintf(stderr, "Eagle: could not find stream information");
        return ret;
    }

    //dump input information
    av_dump_format(p_input_stream_info->p_fmt_ctx, 0, filename, 0);

    return ret;
}

static int decode_write_frame(FILE *pOutput_File, AVCodecContext *avctx,
                              AVFrame *frame, int *frame_count, AVPacket *pkt, int last, 
                              MemInfo *pmeminfo, int filtered_flag, DecodeInfo *pdecinfo)
{
    int i;
    int idx;
    int color_idx;
    int len, got_frame;
    char buf[1024];

    len = avcodec_decode_video2(avctx, frame, &got_frame, pkt);
    if (len < 0) {
        fprintf(stderr, "Error while decoding frame %d\n", *frame_count);
        return len;
    }

    if (got_frame) {

        fflush(stdout);

		if (!filtered_flag) {
			
			av_image_copy(pdecinfo->video_dst_data, pdecinfo->video_dst_linesize,
						  (const uint8_t *)(frame->data), frame->linesize,
						  AV_PIX_FMT_YUV420P, frame->width, frame->height);
			
		    //memcpy(pmeminfo->pDecodeVideoBuffer + (*frame_count) * frame->width * frame->height * 3 / 2,
			//    frame->data[0],	frame->width * frame->height * 3 / 2);
			memcpy(pmeminfo->pDecodeVideoBuffer + (*frame_count) * frame->width * frame->height * 3 / 2,
			    pdecinfo->video_dst_data[0],	frame->width * frame->height * 3 / 2);
		} else {
			av_image_copy(pdecinfo->video_dst_data, pdecinfo->video_dst_linesize,
						  (const uint8_t *)(frame->data), frame->linesize,
						  AV_PIX_FMT_YUV420P, frame->width, frame->height);
			memcpy(pmeminfo->pDecodeVideoBuffer2 + (*frame_count) * frame->width * frame->height * 3 / 2,
			    pdecinfo->video_dst_data[0],	frame->width * frame->height * 3 / 2);
		}

        //the picture is allocated by the decoder, no need to free it
        (*frame_count)++;

        //fwrite(frame->data[0], 1, frame->width*frame->height, pOutput_File);
        //fwrite(frame->data[1], 1, (frame->width/2)*(frame->height/2), pOutput_File);
        //fwrite(frame->data[2], 1, (frame->width/2)*(frame->height/2), pOutput_File);

        if (pkt->data) {
            pkt->size -= len;
            pkt->data += len;
        }
    }

    return got_frame;
}

static int decode_write_frame_new(FILE *pOutput_File, AVCodecContext *avctx,
                              AVFrame *frame, int *frame_count, AVPacket *pkt, int last,
                              MemInfo *pmeminfo, int filtered_flag, DecodeInfo *pdecinfo)
{
	char buf[1024];
	int ret;

	ret = avcodec_send_packet(avctx, pkt);
	if (ret < 0) {
		fprintf(stderr, "Eagle: Error sending a packet for decoding in decode_write_frame_new\n");
		exit(1);
	}

	while (ret >= 0) {
		ret = avcodec_receive_frame(avctx, frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return;
		else if (ret < 0) {
			fprintf(stderr, "Error during decoding\n");
			exit(1);
		}

		fflush(stdout);

		if (!filtered_flag) {
			#if 1
			av_image_copy(pdecinfo->video_dst_data, pdecinfo->video_dst_linesize,
						  (const uint8_t *)(frame->data), frame->linesize,
						  AV_PIX_FMT_YUV420P, frame->width, frame->height);

			memcpy(pmeminfo->pDecodeVideoBuffer + (*frame_count) * frame->width * frame->height * 3 / 2,
			    pdecinfo->video_dst_data[0],	frame->width * frame->height * 3 / 2);
			//memcpy(pmeminfo->pDecodeVideoBuffer + (*frame_count) * frame->width * frame->height * 3 / 2,
			//	(const uint8_t *)(frame->data[0]), frame->width * frame->height);
			//memcpy(pmeminfo->pDecodeVideoBuffer + (*frame_count) * frame->width * frame->height * 3 / 2 + frame->width * frame->height,
			//	(const uint8_t *)(frame->data[1]), (frame->width / 2) * (frame->height / 2));
			//memcpy(pmeminfo->pDecodeVideoBuffer + (*frame_count) * frame->width * frame->height * 3 / 2 + frame->width * frame->height + (frame->width / 2) * (frame->height / 2),
			//	(const uint8_t *)(frame->data[2]), (frame->width / 2) * (frame->height / 2));
			#else
			memcpy(pmeminfo->pDecodeVideoBuffer + (*frame_count) * frame->width * frame->height * 3 / 2,
				(const uint8_t *)(frame->data[0]), frame->width * frame->height);
			memcpy(pmeminfo->pDecodeVideoBuffer + (*frame_count) * frame->width * frame->height * 3 / 2 + frame->width * frame->height,
				(const uint8_t *)(frame->data[1]), (frame->width / 2) * (frame->height / 2));
			memcpy(pmeminfo->pDecodeVideoBuffer + (*frame_count) * frame->width * frame->height * 3 / 2 + frame->width * frame->height + (frame->width / 2) * (frame->height / 2),
				(const uint8_t *)(frame->data[2]), (frame->width / 2) * (frame->height / 2));
			#endif
		} else {
			av_image_copy(pdecinfo->video_dst_data, pdecinfo->video_dst_linesize,
						  (const uint8_t *)(frame->data), frame->linesize,
						  AV_PIX_FMT_YUV420P, frame->width, frame->height);
			memcpy(pmeminfo->pDecodeVideoBuffer2 + (*frame_count) * frame->width * frame->height * 3 / 2,
			    pdecinfo->video_dst_data[0],	frame->width * frame->height * 3 / 2);
			
			//memcpy(pmeminfo->pDecodeVideoBuffer2 + (*frame_count) * frame->width * frame->height * 3 / 2,
			//    pdecinfo->video_dst_data[0],	frame->width * frame->height);
			//memcpy(pmeminfo->pDecodeVideoBuffer2 + (*frame_count) * frame->width * frame->height * 3 / 2 + frame->width * frame->height,
			//	(const uint8_t *)(frame->data[1]), (frame->width / 2) * (frame->height / 2));
			//memcpy(pmeminfo->pDecodeVideoBuffer2 + (*frame_count) * frame->width * frame->height * 3 / 2 + frame->width * frame->height + (frame->width / 2) * (frame->height / 2),
			//	(const uint8_t *)(frame->data[2]), (frame->width / 2) * (frame->height / 2));
		}

        //the picture is allocated by the decoder, no need to free it
        (*frame_count)++;

        //fwrite(frame->data[0], 1, frame->width*frame->height, pOutput_File);
        //fwrite(frame->data[1], 1, (frame->width/2)*(frame->height/2), pOutput_File);
        //fwrite(frame->data[2], 1, (frame->width/2)*(frame->height/2), pOutput_File);

	}
}


static int decode_encoded_h264_rawdata(DecEncH264FmtInfo *pinfo, MemInfo *pmeminfo, DecodeInfo *pdecinfo)
{
	int ret = 0;
	int len = 0, frame_count = 0;

	pinfo->inbuf = (uint8_t *)malloc(INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);
	memset(pinfo->inbuf , 0, INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);
	av_init_packet(&(pinfo->pkt));
	pinfo->codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!pinfo->codec) {
		fprintf(stderr, "cannot find the decoder\n");
		exit(1);
	}
	
	pinfo->codecCtx = avcodec_alloc_context3(pinfo->codec);
	if (!pinfo->codecCtx) {
		fprintf(stderr, "could not allocate video codec context\n");
		exit(1);
	}
	
	if (pinfo->codec->capabilities & AV_CODEC_CAP_TRUNCATED) {
		pinfo->codecCtx->flags |= AV_CODEC_FLAG_TRUNCATED;
	}
	
	pinfo->pCodecParserCtx = av_parser_init(AV_CODEC_ID_H264);
	if (!pinfo->pCodecParserCtx) {
		fprintf(stderr, "Error: alloc parser fail\n");
		exit(1);
	}
	
	//open the decoder
	if (avcodec_open2(pinfo->codecCtx, pinfo->codec, NULL) < 0) {
		fprintf(stderr, "could not open the decoder\n");
		exit(1);
	}
	
	//open frame structure
	pinfo->frame = av_frame_alloc();
	if (!pinfo->frame) {
		fprintf(stderr, "could not allocate video frame\n");
		exit(1);
	}
	
	frame_count = 0;

	memcpy(pinfo->inbuf, pmeminfo->pEncodeVideoBuffer, saved_data_size);
	pinfo->pDataPtr  = pinfo->inbuf;
	pinfo->uDataSize = saved_data_size;
	//printf("saved_data_size %d\n", saved_data_size);
	while (pinfo->uDataSize > 0) {
		len = av_parser_parse2(pinfo->pCodecParserCtx, pinfo->codecCtx, &(pinfo->pkt.data), &(pinfo->pkt.size),
							pinfo->pDataPtr, pinfo->uDataSize,
							AV_NOPTS_VALUE, AV_NOPTS_VALUE, AV_NOPTS_VALUE);
		pinfo->uDataSize -= len;
		pinfo->pDataPtr  += len;

		if (pinfo->pkt.size)
			decode_write_frame_new(pinfo->outputfp, pinfo->codecCtx, pinfo->frame, &frame_count, &(pinfo->pkt), 0, pmeminfo, 0, pdecinfo) ;
	} 

	//decode the data in the decoder itself
	pinfo->pkt.size = 0;
	pinfo->pkt.data = NULL;

	decode_write_frame_new(pinfo->outputfp, pinfo->codecCtx, pinfo->frame, &frame_count, NULL, 0, pmeminfo, 0, pdecinfo);

	return 0;
}

static int compute_vmaf_prepare(struct newData **s, int *vmaf_width, int *vmaf_height, 
										const int width, const int height,
										const uint8_t *ref, const uint8_t *dis)
							//DecEncH264FmtInfo *Dec264FmtInfo, const MemInfo *pmeminfo)
{
	int ret = 0;
	struct newData *ps = (struct newData *)malloc(sizeof(struct newData));
	memset(ps, 0, sizeof(struct newData));
	ps->format   = "yuv420p";
	*vmaf_width  = width;
	*vmaf_height = height;
	ps->width =  *vmaf_width;
	ps->height = *vmaf_height;
	if (!strcmp(ps->format, "yuv420p")) {
		if (((*vmaf_width) * (*vmaf_height)) % 2 != 0) {
			fprintf(stderr, "(width * height) %% 2 != 0, width = %d, height %d.\n", (*vmaf_width), (*vmaf_height));
			ret = 1;
			exit(0);
		}
		ps->offset = (*vmaf_width) * (*vmaf_height) >> 1;
	}
	ps->ref = ref;
	ps->dis = dis;
	ps->num_frames = 50;
	*s = ps;

	return 0;	
}

static int init_video_filter(const char *filter_descr, int width, int height, UnsharpFilterInfo *pfilterinfo)
{
	char args[512];

	AVFilter *buffersrc  = avfilter_get_by_name("buffer");
	AVFilter *buffersink = avfilter_get_by_name("buffersink");
	pfilterinfo->outputs = avfilter_inout_alloc();
	pfilterinfo->inputs  = avfilter_inout_alloc();

	enum AVPixelFormat pix_fmts[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
	AVBufferSinkParams *buffersink_params;

	pfilterinfo->filter_graph = avfilter_graph_alloc();

	//buffers video source: the decoded frames from the decoder will be inserted here.
	snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", width, height, AV_PIX_FMT_YUV420P, 1, 25, 1, 1);
	int ret = avfilter_graph_create_filter(&(pfilterinfo->buffersrc_ctx), buffersrc, "in", args, NULL, pfilterinfo->filter_graph);
	if (ret < 0) {
		printf("Error: cannot create buffer source.\n");
		return ret;
	}

	//buffer video sink: to terminate the filter chain
	buffersink_params = av_buffersink_params_alloc();
	buffersink_params->pixel_fmts = pix_fmts;

	ret = avfilter_graph_create_filter(&(pfilterinfo->buffersink_ctx), buffersink, "out", NULL, buffersink_params, pfilterinfo->filter_graph);;
	av_free(buffersink_params);
	if (ret < 0) {
		printf("error: cannot create buffer sink\n");
		return ret;
	}

	//encpoints for the filter graph.
	pfilterinfo->outputs->name       = av_strdup("in");
	pfilterinfo->outputs->filter_ctx = pfilterinfo->buffersrc_ctx;
	pfilterinfo->outputs->pad_idx    = 0;
	pfilterinfo->outputs->next       = NULL;

	pfilterinfo->inputs->name       = av_strdup("out");
	pfilterinfo->inputs->filter_ctx = pfilterinfo->buffersink_ctx;
	pfilterinfo->inputs->pad_idx    = 0;
	pfilterinfo->inputs->next       = NULL;

    if ((ret = avfilter_graph_parse_ptr(pfilterinfo->filter_graph, filter_descr, &(pfilterinfo->inputs), &(pfilterinfo->outputs), NULL)) < 0) {
        printf("error: avfilter_graph_parse_ptr failed\n");
        return ret;
    }

    if ((ret = avfilter_graph_config(pfilterinfo->filter_graph, NULL)) < 0) {
        printf("error: avfilter_graph_config\n");
        return ret;
    }

    return 0;	
}

static int add_frame_to_filter(AVFrame *frameIn, UnsharpFilterInfo *pfilterinfo)
{
	if (av_buffersrc_add_frame(pfilterinfo->buffersrc_ctx, frameIn) < 0) {
		return 0;
	}
	return 1;
}

static int get_frame_from_filter(AVFrame **frameOut, UnsharpFilterInfo *pfilterinfo)
{
	if (av_buffersink_get_frame(pfilterinfo->buffersink_ctx, *frameOut) < 0){
		return 0;
	}
	return 1;
}

static void init_video_frame_in_out(AVFrame **frameIn, AVFrame **frameOut, unsigned char **frame_buffer_in, unsigned char **frame_buffer_out, int frameWidth, int frameHeight)
{
    *frameIn = av_frame_alloc();
    *frame_buffer_in = (unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, frameWidth, frameHeight, 1));
    av_image_fill_arrays((*frameIn)->data, (*frameIn)->linesize, *frame_buffer_in, AV_PIX_FMT_YUV420P, frameWidth, frameHeight, 1);

    *frameOut = av_frame_alloc();
    *frame_buffer_out = (unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, frameWidth, frameHeight, 1));
    av_image_fill_arrays((*frameOut)->data, (*frameOut)->linesize, *frame_buffer_out, AV_PIX_FMT_YUV420P, frameWidth, frameHeight, 1);

    (*frameIn)->width  = frameWidth;
    (*frameIn)->height = frameHeight;
    (*frameIn)->format = AV_PIX_FMT_YUV420P;
}

static int read_yuv_data_to_buf(unsigned char *frame_buffer_in, const uint8_t *data, AVFrame **frameIn, int width, int height, int first_part)
{
	static int filter_frame_num = 0;
	static int filter_frame_num_two = 0;
	AVFrame *pFrameIn = *frameIn;
	int frameSize = width * height * 3 / 2;


	if (first_part) {
		if(filter_frame_num < DECODE_FRAME_NUM_PER_GOP) {
			memcpy(frame_buffer_in, data + filter_frame_num * width * height * 3 / 2, frameSize);
		} else {
			filter_frame_num = 0;
			return 0;
		}
	} else {
		if(filter_frame_num_two < DECODE_FRAME_NUM_PER_GOP) {
			memcpy(frame_buffer_in, data + filter_frame_num_two * width * height * 3 / 2, frameSize);
		} else {
			filter_frame_num_two = 0;
			return 0;
		}
	}

	pFrameIn->data[0] = frame_buffer_in;
	pFrameIn->data[1] = pFrameIn->data[0] + width * height;
	pFrameIn->data[2] = pFrameIn->data[1] + width * height / 4;

	if (first_part) {
		filter_frame_num++;
		filter_frame_num = (filter_frame_num == DECODE_FRAME_NUM_PER_GOP) ? 0 : filter_frame_num;
	} else {
		filter_frame_num_two++;
		filter_frame_num_two = (filter_frame_num_two == FILTERED_FRAME_NUM_PER_GOP) ? 0 : filter_frame_num_two;
	}
	
	return 1;
}

static int read_yuv_data_to_buf_two(unsigned char *frame_buffer_in, const uint8_t *data, AVFrame **frameIn, int width, int height)
{
	static int filter_frame_num_two = 0;
	AVFrame *pFrameIn = *frameIn;
	int frameSize = width * height * 3 / 2;

	//if (fread(frame_buffer_in, 1, frameSize, files.iFile) != frameSize) {
	//	return 0;
	//}

	//printf("filter_frame_num %d\n", filter_frame_num);

	if(filter_frame_num_two < DECODE_FRAME_NUM_PER_GOP) {
		//printf("filter_frame_num_two %d line %d\n", filter_frame_num_two, __LINE__);
		memcpy(frame_buffer_in, data + filter_frame_num_two * width * height * 3 / 2, frameSize);
	} else {
		//printf("filter_frame_num_two %d line %d\n", filter_frame_num_two, __LINE__);
		filter_frame_num_two = 0;
		return 0;
	}

	pFrameIn->data[0] = frame_buffer_in;
	pFrameIn->data[1] = pFrameIn->data[0] + width * height;
	pFrameIn->data[2] = pFrameIn->data[1] + width * height / 4;

	filter_frame_num_two++;
	
	return 1;
}


static void write_yuv_to_outfile(const AVFrame *frame_out, FILE *fp)
{
	if (frame_out->format == AV_PIX_FMT_YUV420P) {
		for (int i = 0; i < frame_out->height; i++) {
			fwrite(frame_out->data[0] + frame_out->linesize[0] * i, 1, frame_out->width, fp);;
		}
        for (int i = 0; i < frame_out->height >> 1; i++) {
            fwrite(frame_out->data[1] + frame_out->linesize[1] * i, 1, frame_out->width >> 1, fp);
        }
        for (int i = 0; i < frame_out->height >> 1; i++) {
            fwrite(frame_out->data[2] + frame_out->linesize[2] * i, 1, frame_out->width >> 1, fp);
        }
	}
}

static int unsharp_decoded_yuv(UnsharpFilterInfo *pfilterinfo, const MemInfo *pmeminfo, const InputStreamInfo *p_input_stream_info, FILE *fp_filter, int first_part)
{
	int ret = 0;
	int frameWidth  = p_input_stream_info->p_frame->width;
	int frameHeight = p_input_stream_info->p_frame->height;
	uint8_t *data = NULL;

	printf("filter_descr %s frameWidth %d frameHeight %d\n", pfilterinfo->filter_descr, frameWidth, frameHeight);
	if (ret = init_video_filter(pfilterinfo->filter_descr, frameWidth, frameHeight, pfilterinfo)) {
		return ret;
	};

	init_video_frame_in_out(&(pfilterinfo->frame_in), &(pfilterinfo->frame_out), &(pfilterinfo->frame_buffer_in), &(pfilterinfo->frame_buffer_out), frameWidth, frameHeight);

	if (first_part) {
		data = pmeminfo->pVideoBufferCrf5;
	} else {
		data = pmeminfo->pVideoBuffer;
	}

	while (read_yuv_data_to_buf(pfilterinfo->frame_buffer_in, data, &(pfilterinfo->frame_in), frameWidth, frameHeight, first_part)) {
		//add frame to filter graph
		if (!add_frame_to_filter((pfilterinfo->frame_in), pfilterinfo)) {
			printf("error: while adding frame\n");
			goto end;
		}

		//get frame from filter graph
		if (!get_frame_from_filter(&(pfilterinfo->frame_out), pfilterinfo)) {
			printf("error: while getting frame\n");
			goto end;
		}

		//write the frame to output file
		//if (first_part)
		//	write_yuv_to_outfile(pfilterinfo->frame_out, fp_filter);

		if (first_part)
		{
			memcpy(pmeminfo->pVideoBuffer1 + pfilterinfo->filtered_frame_num * pfilterinfo->frame_out->width * pfilterinfo->frame_out->height * 3 / 2, 
					pfilterinfo->frame_out->data[0], 
					pfilterinfo->frame_out->width * pfilterinfo->frame_out->height);
			memcpy(pmeminfo->pVideoBuffer1 + pfilterinfo->filtered_frame_num * pfilterinfo->frame_out->width * pfilterinfo->frame_out->height * 3 / 2 + pfilterinfo->frame_out->width * pfilterinfo->frame_out->height,
					pfilterinfo->frame_out->data[1], 
					(pfilterinfo->frame_out->width >> 1) * (pfilterinfo->frame_out->height >> 1));
			memcpy(pmeminfo->pVideoBuffer1 + pfilterinfo->filtered_frame_num * pfilterinfo->frame_out->width * pfilterinfo->frame_out->height * 3 / 2 + pfilterinfo->frame_out->width * pfilterinfo->frame_out->height + (pfilterinfo->frame_out->width >> 1) * (pfilterinfo->frame_out->height >> 1),
					pfilterinfo->frame_out->data[2], 
					(pfilterinfo->frame_out->width >> 1) * (pfilterinfo->frame_out->height >> 1));

		} else {
			memcpy(pmeminfo->pVideoBuffer2 + pfilterinfo->filtered_frame_num * pfilterinfo->frame_out->width * pfilterinfo->frame_out->height * 3 / 2, 
					pfilterinfo->frame_out->data[0], 
					pfilterinfo->frame_out->width * pfilterinfo->frame_out->height);
			memcpy(pmeminfo->pVideoBuffer2 + pfilterinfo->filtered_frame_num * pfilterinfo->frame_out->width * pfilterinfo->frame_out->height * 3 / 2 + pfilterinfo->frame_out->width * pfilterinfo->frame_out->height,
					pfilterinfo->frame_out->data[1], 
					(pfilterinfo->frame_out->width >> 1) * (pfilterinfo->frame_out->height >> 1));
			memcpy(pmeminfo->pVideoBuffer2 + pfilterinfo->filtered_frame_num * pfilterinfo->frame_out->width * pfilterinfo->frame_out->height * 3 / 2 + pfilterinfo->frame_out->width * pfilterinfo->frame_out->height + (pfilterinfo->frame_out->width >> 1) * (pfilterinfo->frame_out->height >> 1),
					pfilterinfo->frame_out->data[2], 
					(pfilterinfo->frame_out->width >> 1) * (pfilterinfo->frame_out->height >> 1));
		}
		av_frame_unref(pfilterinfo->frame_out);
		pfilterinfo->filtered_frame_num++;
		if (first_part) {
			if (pfilterinfo->filtered_frame_num == DECODE_FRAME_NUM_PER_GOP) {
				pfilterinfo->filtered_frame_num = 0;
				break;
			}
		} else if (pfilterinfo->filtered_frame_num == FILTERED_FRAME_NUM_PER_GOP) {
			pfilterinfo->filtered_frame_num = 0;
			break;
		}
	}

	avfilter_graph_free(&(pfilterinfo->filter_graph));
	avfilter_inout_free(&(pfilterinfo->outputs));
	avfilter_inout_free(&(pfilterinfo->inputs));
	av_frame_free(&(pfilterinfo->frame_in));
	av_frame_free(&(pfilterinfo->frame_out));
	av_free(pfilterinfo->frame_buffer_in);
	av_free(pfilterinfo->frame_buffer_out);

	return 0;
end:
	av_frame_free(&(pfilterinfo->frame_in));
	av_frame_free(&(pfilterinfo->frame_out));
	return 0;
}

static int enc_filtered_yuv_to_264(MemInfo *pmeminfo, float crf_val, const InputStreamInfo *p_input_stream_info, int fps)
{
	int ret = 0;
	static int enc_frame_num = 0;

	AVFrame *pframe;
	AVPacket *ppkt;
	AVPacket pkt;
	X264Context *x4 = NULL;
	
	enum AVCodecID codec_id = AV_CODEC_ID_H264;
	AVCodec *pcodec = avcodec_find_encoder(codec_id);
	if (!pcodec) {
		fprintf(stderr, "enc_filtered_yuv_to_264\n");
		return -1;
	}

	AVCodecContext *pcodecCtx = avcodec_alloc_context3(pcodec);
	if (!pcodecCtx) {
		fprintf(stderr, "Eagle: could not allocate video codec context\n");
		return -1;
	}

	pcodecCtx->width     = p_input_stream_info->p_frame->width;
	pcodecCtx->height    = p_input_stream_info->p_frame->height;
	pcodecCtx->pix_fmt   = AV_PIX_FMT_YUV420P;
	pcodecCtx->time_base = (AVRational){1, fps};
	pcodecCtx->framerate = (AVRational){fps, 1};

	av_opt_set(pcodecCtx->priv_data, "profile", "high",   0);
	av_opt_set(pcodecCtx->priv_data, "preset",  "medium", 0);
	av_opt_set(pcodecCtx->priv_data, "tune",    "ssim",   0);

	//open the encoder
	if (avcodec_open2(pcodecCtx, pcodec, NULL) < 0) {
		fprintf(stderr, "Eagle: Open encoder fail\n");
		return -1;
	}

	//allocate AVFrame structure
	pframe = av_frame_alloc();
	if (!pframe) {
		fprintf(stderr, "Eagle: could not allocate video frame\n");
		return -1;
	}

	pframe->width  = pcodecCtx->width;
	pframe->height = pcodecCtx->height;
	pframe->format = pcodecCtx->pix_fmt;
	pframe->linesize[0] = p_input_stream_info->p_frame->linesize[0];
	pframe->linesize[1] = p_input_stream_info->p_frame->linesize[1];
	pframe->linesize[2] = p_input_stream_info->p_frame->linesize[2];

	//allocate AVFrame data
	ret = av_frame_get_buffer(pframe, 32);
	if (ret < 0) {
		fprintf(stderr, "Eagle: could not allocate the video frame data\n");
		return ret;
	}

	//init AVPacket
	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;

	ppkt = av_packet_alloc();
	if (!ppkt) {
		fprintf(stderr, "Eagle: could not allocate pkt\n");
		return -1;
	}

	{
		//reconfig encoder params(aq_strength, crf), using the x264_encoder_reconfig
		X264Context *x4 = pcodecCtx->priv_data;
		x4->crf = crf_val;
		x4->params.rc.f_rf_constant = crf_val;
		//x4->aq_strength = global_aq_strength_array[global_stage2_gop_num];
		//x4->params.rc.f_aq_strength = global_aq_strength_array[global_stage2_gop_num];
		x264_encoder_reconfig(x4->enc, &(x4->params));
	}

	//encode frame
	for (int i = 0; i < 6/*FILTERED_FRAME_NUM_PER_GOP/*6*/; i++) {
		fill_yuv_image_filtered(pframe->data, pframe->linesize, 
								pcodecCtx->width, pcodecCtx->height, i, pmeminfo);
		pframe->pts = i;

		//encode the image
		ret = avcodec_send_frame(pcodecCtx, pframe);
		if (ret == AVERROR_EOF) {
			break;
		} else if (ret < 0) {
			fprintf(stderr, "Eagle: Error sending a frame for encoding\n");
			return ret;
		}

		while (ret >= 0) {
			ret = avcodec_receive_packet(pcodecCtx, ppkt);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
				//fprintf(stderr, "Eagle: could not a receive packet AVERROR_EOF %x ret %x\n",
				//				AVERROR_EOF, AVERROR(EAGAIN), ret);
				continue;
			} else if (ret < 0) {
				fprintf(stderr, "Eagle: error during encoding\n");
				return ret;
			}
			//printf("encoding success\n");

			memcpy(pmeminfo->pEncodeVideoBuffer2 + saved_data_size_filtered, ppkt->data, ppkt->size);
			saved_data_size_filtered += ppkt->size;
			enc_pkt_size_filtered[enc_frame_num++] = ppkt->size;

			av_packet_unref(ppkt);
		}
	}

	//flush
	ret = avcodec_send_frame(pcodecCtx, NULL);
	if (ret < 0) {
		fprintf(stderr, "Eagle: error sending a frame for encoding\n");
		exit(1);
	}

	while (ret >= 0) {
		ret = avcodec_receive_packet(pcodecCtx, ppkt);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			break;
		} else if (ret < 0) {
			fprintf(stderr, "Eagle: error during encoding\n");
			exit(1);
		}

		//FILE *fp = fopen("./encode264_filtered.264", "ab");
		//fwrite(ppkt->data, 1, ppkt->size, fp);
		//fclose(fp);

		memcpy(pmeminfo->pEncodeVideoBuffer2 + saved_data_size_filtered, ppkt->data, ppkt->size);
		saved_data_size_filtered += ppkt->size;
		enc_pkt_size_filtered[enc_frame_num++] = ppkt->size;

		av_packet_unref(ppkt);
	}
	//fprintf(stderr, "flush success filtered yuv to 264\n");

	//uninit the encoder
	enc_frame_num = 0;
	av_packet_free(&ppkt);
	av_frame_free(&pframe);
	avcodec_close(pcodecCtx);
	avcodec_free_context(&pcodecCtx);

	return 0;
}

static int decode_filtered_encoded_h264_rawdata(MemInfo *pmeminfo, DecodeInfo *pdecinfo)
{
	int len = 0, frame_count = 0;
	uint8_t *pDataPtr = NULL;
	size_t uDataSize;
	FILE *fp = NULL;//fopen("dec_filtered_enc.yuv", "wb");
	AVCodecParserContext * pcodecparsctx = NULL;
	AVFrame *pframe = NULL;
	AVPacket pkt;
	uint8_t *inbuf = (uint8_t *)malloc(INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);
	memset(inbuf, 0, INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);
	av_init_packet(&pkt);
	AVCodec *pcodec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!pcodec) {
		fprintf(stderr, "cannot find the decoder in the filtered encode part\n");
		exit(1);
	}

	AVCodecContext *pcodecctx = avcodec_alloc_context3(pcodec);
	if (!pcodecctx) {
		fprintf(stderr, "could not allocate video codec context in the filtered encode part\n");
		exit(1);
	}

	if (pcodec->capabilities & AV_CODEC_CAP_TRUNCATED) {
		pcodecctx->flags |= AV_CODEC_FLAG_TRUNCATED;
	}
	
	pcodecparsctx = av_parser_init(AV_CODEC_ID_H264);
	if (!pcodecparsctx) {
		fprintf(stderr, "Error: alloc parser fail\n");
		exit(1);
	}
	
	//open the decoder
	if (avcodec_open2(pcodecctx, pcodec, NULL) < 0) {
		fprintf(stderr, "could not open the decoder\n");
		exit(1);
	}
	
	//open frame structure
	pframe = av_frame_alloc();
	if (!pframe) {
		fprintf(stderr, "could not allocate video frame\n");
		exit(1);
	}

	frame_count = 0;

	memcpy(inbuf, pmeminfo->pEncodeVideoBuffer2, saved_data_size_filtered);
	pDataPtr  = inbuf;
	uDataSize = saved_data_size_filtered;
	while (uDataSize > 0) {
		len = av_parser_parse2(pcodecparsctx, pcodecctx, &(pkt.data), &(pkt.size),
							pDataPtr, uDataSize, AV_NOPTS_VALUE, AV_NOPTS_VALUE, AV_NOPTS_VALUE);
		uDataSize -= len;
		pDataPtr  += len;

		if (pkt.size == 0) {
			continue;
		}

		if (decode_write_frame(fp, pcodecctx, pframe, &frame_count, &pkt, 0, pmeminfo, 1, pdecinfo) < 0) {
			exit(1);
		}
	}

	//decode the data in the decoder itself
	pkt.size = 0;
	pkt.data = NULL;
	decode_write_frame(fp, pcodecctx, pframe, &frame_count, &pkt, 0, pmeminfo, 1, pdecinfo);

	//uninit the decoder
	if (fp)
		fclose(fp);
	av_frame_free(&pframe);
	avcodec_close(pcodecctx);
	av_parser_close(pcodecparsctx);
	avcodec_free_context(&pcodecctx);
	free(inbuf);
	inbuf = NULL;	
	
	return 0;
}

static float get_unsharp(float pixel_unsharpness)
{
	float unsharp_factor;
	if (pixel_unsharpness <= 0.1)
		pixel_unsharpness = 0.1;
	else if (pixel_unsharpness >= 0.8)
		pixel_unsharpness = 0.8;

	unsharp_factor = (((0.8 - pixel_unsharpness) / 0.7) * ((0.8 - pixel_unsharpness) / 0.7)) * 6;

	if (unsharp_factor <= 1.0)
		unsharp_factor = 1.0;

	return unsharp_factor * pixel_unsharpness;
}

static float get_aq_strength(float pixel_unsharpness)
{
	float aq_float = 0.0;
	if (pixel_unsharpness <= 0.1)
		pixel_unsharpness = 0.1;
	else if (pixel_unsharpness >= 0.8)
		pixel_unsharpness = 0.8;

	aq_float = (float)(0.5 + (0.8 - pixel_unsharpness) / 0.7);
	if (aq_float < 1.0)
		aq_float = 1.0;

	return aq_float;
}

//decode the mp4 format h264 codec to yuv,
static int EaglePreProcess(char *filename)
{
	int fps = 0;
	int org_bitrate = 0;
	int end_of_file = 0;
	const char *fmt = "yuv420p";
	char *log_file = "./log_test.txt";
	char *log_file_2 = "./log_test_2.txt";
	int vmaf_width, vmaf_height;
	char *model_path = "/usr/local/share/model/vmaf_v0.6.1.pkl";
	const char *pool_method = NULL;
	int frame_num = 0;
    int ret = -1;
	double vmaf_score = 0.0;
	float stage2_score_in = 0.0;
	float stage2_score_diff = 0.0;
	float stage2_crf_step = 1.0;
	float stage2_bitrate_in = 0.0;
	float stage2_step_vmaf = 0.0;
	float stage2_vmaf_diff = 0.0;
	float stage2_step_vmaf_res = 0.0;
	float stage2_best_bitrate = 1000000;
	float stage2_best_vmaf_diff = 100;
	float stage2_best_vmaf = 0;
	float stage2_target_vmaf_score = 0.0;
	int   stage2_best_crf  = 0;
	int   stage2_early_stop = 0;
	int   stage2_loop_count = 0;
	int   stage2_resolution_num = 1;
	int   stage2_first_flag = 1;
	int   stage2_last_crf = 18;
	int   stage2_start_crf = 18;
	struct newData *s = NULL;
	int disable_clip = 0, disable_avx = 0, enable_transform = 0, phone_model = 0;
	int do_psnr = 0, do_ssim = 0, do_ms_ssim = 0, n_thread = 1, n_subsample = 1, enable_conf_interval = 0; 
    int video_stream_idx = -1, audio_stream_idx = -1;

	struct timeval before_crf5_part,  after_crf5_part  = {0};
	long long crf5_time_val = 0;
	struct timeval before_loop1_part, after_loop1_part = {0};
	long long loop1_time_val = 0;
	struct timeval before_loop2_part, after_loop2_part = {0};
	long long loop2_time_val;

	//crf & bitrate & target_score
	float stage1_prev_bitrate = 0.0, stage1_bitrate = 0.0;
	float stage2_prev_bitrate = 0.0, stage2_bitrate = 0.0;
	float stage1_vmaf_score = 0.0, stage1_prev_vmaf_score = 0.0;
	float stage2_vmaf_score = 0.0, stage2_prev_vmaf_score = 0.0;
	float stage1_per_score = 0.0, stage2_per_score = 0.0;
	int stage1_crf = 0, stage2_crf = 0;
	float target_per_score = 400;
	static int stage1_first_flag = 1;

	//to determin the unsharp value
	float pre_vmaf_score = 0.0;
	int has_checked = 0;
	
	char *unsharp_val[10] = {"0.0", "0.1", "0.2", "0.3", "0.4", "0.5", "0.6", "0.7", "0.8", "0.9"};
	float unsharp[10]     = {0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9};

    DecodeInfo *pdecinfo = (DecodeInfo *)malloc(sizeof(DecodeInfo));
	if (pdecinfo != NULL) {
		memset(pdecinfo, 0, sizeof(DecodeInfo));
	}
    EncodeInfo *pencinfo = (EncodeInfo *)malloc(sizeof(EncodeInfo));
	if (pencinfo != NULL) {
		memset(pencinfo, 0, sizeof(EncodeInfo));
	}
    InputParams inputpar;
	DecEncH264FmtInfo *pdec264fmtinfo = (DecEncH264FmtInfo *)malloc(sizeof(DecEncH264FmtInfo));
	if (pdec264fmtinfo != NULL) {
		memset(pdec264fmtinfo, 0, sizeof(DecEncH264FmtInfo));
	}

	UnsharpFilterInfo *pfilterinfo = (UnsharpFilterInfo *)malloc(sizeof(UnsharpFilterInfo));
	if (pfilterinfo != NULL) {
		memset(pfilterinfo, 0, sizeof(UnsharpFilterInfo));
	}

	UnsharpFilterInfo *pfilterinfoOne = (UnsharpFilterInfo *)malloc(sizeof(UnsharpFilterInfo));
	if (pfilterinfoOne!= NULL) {
		memset(pfilterinfoOne, 0, sizeof(UnsharpFilterInfo));
	}

	MemInfo *pmeminfo = (MemInfo *)malloc(sizeof(MemInfo));
	memset(pmeminfo, 0, sizeof(MemInfo));

	//filter part
	unsigned char *frame_buffer_in  = NULL;
	unsigned char *frame_buffer_out = NULL;
	int frameWidth, frameHeight = 0;
	int filtered_frame_num = 0;
	long long sharpness = 0;
	long long total_sharpness = 0;

	pdec264fmtinfo->outputfp = NULL;//(FILE *)fopen("./end.yuv", "wb");
	if (pdec264fmtinfo->outputfp == NULL) {
		//printf("open end yuv fail\n");
	}
    inputpar.video_dst_file = NULL;//(FILE *)fopen("./output.yuv", "wb");

	FILE *fp_filter = NULL;//(FILE *)fopen("./filter.yuv", "wb+");
	//strcpy(pfilterinfo->filter_descr, "unsharp=luma_msize_x=5:luma_msize_y=5:luma_amount=0.9");
	//strcpy(pfilterinfoOne->filter_descr, "unsharp=luma_msize_x=5:luma_msize_y=5:luma_amount=0.8");
	//pfilterinfo->filter_descr    = "unsharp=luma_msize_x=5:luma_msize_y=5:luma_amount=0.9";
	//pfilterinfoOne->filter_descr = "unsharp=luma_msize_x=5:luma_msize_y=5:luma_amount=0.4";
	//strcpy(pfilterinfo->filter_descr, "unsharp=luma_msize_x=5:luma_msize_y=5:luma_amount=0.9");
	strcat(pfilterinfoOne->filter_descr, "unsharp=luma_msize_x=5:luma_msize_y=5:luma_amount=");

	//allocate meminfo
	pmeminfo->pVideoBuffer 			= (uint8_t *)malloc(FHD_BUFFER_SIZE);
	pmeminfo->pVideoBufferCrf5      = (uint8_t *)malloc(FHD_BUFFER_SIZE);
	
	pmeminfo->pVideoBuffer1         = (uint8_t *)malloc(FHD_BUFFER_SIZE);
	pmeminfo->pEncodeVideoBuffer 	= (uint8_t *)malloc(1024*1024*10);
	pmeminfo->pDecodeVideoBuffer 	= (uint8_t *)malloc(FHD_BUFFER_SIZE);
	
	pmeminfo->pVideoBuffer2			= (uint8_t *)malloc(FHD_BUFFER_SIZE / 5);
	pmeminfo->pEncodeVideoBuffer2 	= (uint8_t *)malloc(1024*1024*10);
	pmeminfo->pDecodeVideoBuffer2 	= (uint8_t *)malloc(FHD_BUFFER_SIZE / 5);

	printf("pVideoBuffer2 %p pEncodeVideoBuffer2 %p pDecodeVideoBuffer2 %p\n",
		pmeminfo->pVideoBuffer2, pmeminfo->pEncodeVideoBuffer2, pmeminfo->pDecodeVideoBuffer2);

	memset(pmeminfo->pVideoBuffer, 		 	0, FHD_BUFFER_SIZE);
	memset(pmeminfo->pVideoBufferCrf5,      0, FHD_BUFFER_SIZE);
	memset(pmeminfo->pVideoBuffer1,         0, FHD_BUFFER_SIZE);
	memset(pmeminfo->pEncodeVideoBuffer, 	0, 1024*1024*10);
	memset(pmeminfo->pDecodeVideoBuffer, 	0, FHD_BUFFER_SIZE);
	memset(pmeminfo->pVideoBuffer2, 		0, FHD_BUFFER_SIZE / 5);
	memset(pmeminfo->pEncodeVideoBuffer2, 	0, 1024*1024*10);
	memset(pmeminfo->pDecodeVideoBuffer2, 	0, FHD_BUFFER_SIZE / 5);

    InputStreamInfo *p_input_stream_info = (InputStreamInfo *)malloc(sizeof(InputStreamInfo));
	InputStreamInfo *p_temp_input_stream_info = (InputStreamInfo *)malloc(sizeof(InputStreamInfo));
    if (!p_input_stream_info) {
        fprintf(stderr, "Eagle:allocate input stream info fail\n");
        return -1;
    } else {
        memset(p_input_stream_info, 0, sizeof(InputStreamInfo));
    }

	//1. handle the input mp4 file, to get some information about the stream
    if ((ret = get_input_fmt(&p_input_stream_info, filename)) < 0) {
        fprintf(stderr, "Eagle: get input format info fail\n");
        return ret;
    }
	printf("p_input_stream_info %p bitrate %d\n", p_input_stream_info, p_input_stream_info->p_fmt_ctx->bit_rate);
	org_bitrate = p_input_stream_info->p_fmt_ctx->bit_rate / 1000;

	fps = ceil(p_input_stream_info->p_fmt_ctx->streams[0]->avg_frame_rate.num / (double)p_input_stream_info->p_fmt_ctx->streams[0]->avg_frame_rate.den);
	printf("avg_frame_rate %d %d fps %d\n", p_input_stream_info->p_fmt_ctx->streams[0]->avg_frame_rate.den,
		p_input_stream_info->p_fmt_ctx->streams[0]->avg_frame_rate.num, fps);

	if (p_input_stream_info->p_fmt_ctx->streams[0]->avg_frame_rate.num == 0) {
		fps = (p_input_stream_info->p_fmt_ctx->streams[0]->r_frame_rate.num / (double)p_input_stream_info->p_fmt_ctx->streams[0]->r_frame_rate.den);
		printf("avg_frame_rate %d %d fps %d\n", p_input_stream_info->p_fmt_ctx->streams[0]->r_frame_rate.den,
			p_input_stream_info->p_fmt_ctx->streams[0]->r_frame_rate.num, fps);
		
	}

    if ((ret = open_codecs_and_contexts(&p_input_stream_info)) != 0) {
        fprintf(stderr, "Eagle: oepn codec and contexts fail\n");
        return ret;
    }

	//2. decode the codec data(h264) to yuv data
    if ((ret = decode_prepare(&p_input_stream_info, pdecinfo)) < 0) {
        fprintf(stderr, "Eagle: decode prepare fail\n");
        return ret;
    }

	while (av_read_frame(p_input_stream_info->p_fmt_ctx, p_input_stream_info->p_pkt) >= 0) {
		do {
			if (p_input_stream_info->p_pkt->stream_index == p_input_stream_info->video_stream_idx) {
				ret = avcodec_send_packet(p_input_stream_info->p_video_codecctx, p_input_stream_info->p_pkt);
				if (ret != 0) {
					fprintf(stderr, "ret %x AVERROR(EAGAIN) %x AVERROR_EOF %x AVERROR(EINVAL) %x AVERROR(ENOMEM) %x\n", 
							ret, AVERROR(EAGAIN), AVERROR_EOF, AVERROR(EINVAL), AVERROR(ENOMEM));
					fprintf(stderr, "Eagle: Error sending a packet for decoding\n");
					return ret;
				}

				while (ret >= 0) {
					ret = avcodec_receive_frame(p_input_stream_info->p_video_codecctx, p_input_stream_info->p_frame);
					if (ret == AVERROR(EAGAIN)) {
						break;
					} else if (ret == AVERROR_EOF) {
						fprintf(stderr, "Eagle: Receive frame error AVERROR_EOF\n");
						end_of_file = 1;
						break;
					} else if (ret < 0) {
						fprintf(stderr, "Eagle: Error during decoding\n");
						break;
					}

					if (p_input_stream_info->p_frame->pict_type == AV_PICTURE_TYPE_I &&
						pdecinfo->dec_frame_num >= MIN_NUM_OF_PER_GOP) {
						pixel_sharpness_val = (float)((float)total_sharpness / pdecinfo->dec_frame_num)/(float)(p_input_stream_info->p_frame->width)/(float)(p_input_stream_info->p_frame->height);
						global_frames_of_gop_array[global_decode_gop_num] = pdecinfo->dec_frame_num;
						global_aq_strength_array[global_decode_gop_num]   = get_aq_strength(pixel_sharpness_val);
						global_unsharp_array[global_decode_gop_num]     = get_unsharp(pixel_sharpness_val);
						printf("gop %d dec_frame_num %lld total_sharpness %lld avg_unsharp %lld pixel_sharpness_val %f unsharp_value %f aq_strength %f\n", 
							global_decode_gop_num,
							pdecinfo->dec_frame_num, total_sharpness, total_sharpness / pdecinfo->dec_frame_num,  
							pixel_sharpness_val, global_unsharp_array[global_decode_gop_num],
							global_aq_strength_array[global_decode_gop_num]);
						total_sharpness         = 0;
						pdecinfo->dec_frame_num = 0;
						p_input_stream_info->p_pkt->data += p_input_stream_info->p_pkt->size;
						p_input_stream_info->p_pkt->size = 0;
						//global_decode_gop_num++;
						total_gop_num++;
						goto NEXT;
					}

DECODE_ORG_BITS:
					av_image_copy(pdecinfo->video_dst_data, pdecinfo->video_dst_linesize,
								  (const uint8_t *)(p_input_stream_info->p_frame->data),
								  p_input_stream_info->p_frame->linesize,
								  pdecinfo->pix_fmt, pdecinfo->width, pdecinfo->height);
					sharpness = get_unsharp_val(pdecinfo->video_dst_data[0],
									pdecinfo->width, pdecinfo->height, 1.0, 5, 5);
					//printf("frame_num %d sharpness %d\n", pdecinfo->dec_frame_num, sharpness);
					total_sharpness += sharpness;

					if (pdecinfo->dec_frame_num <= DECODE_FRAME_NUM_PER_GOP) {
						memcpy(pmeminfo->pVideoBuffer + pdecinfo->dec_frame_num * pdecinfo->width * pdecinfo->height * 3 / 2,
								pdecinfo->video_dst_data[0], pdecinfo->width * pdecinfo->height * 3 / 2);
						//fwrite(pdecinfo->video_dst_data[0], 1, pdecinfo->video_dst_bufsize, inputpar.video_dst_file);
					}

					pdecinfo->dec_frame_num++;
					break;
				}
			}else if (p_input_stream_info->p_pkt->stream_index == p_input_stream_info->audio_stream_idx) {
				//donnot handle audio stream
			}
			ret = p_input_stream_info->p_pkt->size;
			p_input_stream_info->p_pkt->size -= ret;
			p_input_stream_info->p_pkt->data += ret;
		} while (p_input_stream_info->p_pkt->size > 0);
		av_packet_unref(p_input_stream_info->p_pkt);
	}

	end_of_file = 1;

	if (1)
	{
		#if 0
		memcpy(p_temp_input_stream_info, p_input_stream_info, sizeof(InputStreamInfo));
		ret = avcodec_send_packet(p_input_stream_info->p_video_codecctx, NULL);
		if (ret != 0) {
			fprintf(stderr, "ret %x AVERROR(EAGAIN) %x AVERROR_EOF %x AVERROR(EINVAL) %x AVERROR(ENOMEM) %x\n", 
					ret, AVERROR(EAGAIN), AVERROR_EOF, AVERROR(EINVAL), AVERROR(ENOMEM));
			fprintf(stderr, "Eagle: Error sending a packet for decoding line %d\n", __LINE__);
			return ret;
		}

		while (ret == 0) {
			ret = avcodec_receive_frame(p_input_stream_info->p_video_codecctx, p_input_stream_info->p_frame);
			if (ret == AVERROR(EAGAIN)) {
				break;
			} else if (ret == AVERROR_EOF) {
				fprintf(stderr, "Eagle: Receive frame error AVERROR_EOF line %d\n", __LINE__);
				end_of_file = 1;
				break;
			} else if (ret < 0) {
				fprintf(stderr, "Eagle: Error during decoding\n");
				break;
			}

			av_image_copy(pdecinfo->video_dst_data, pdecinfo->video_dst_linesize,
						  (const uint8_t *)(p_input_stream_info->p_frame->data),
						  p_input_stream_info->p_frame->linesize,
						  pdecinfo->pix_fmt, pdecinfo->width, pdecinfo->height);
			sharpness = get_unsharp_val(pdecinfo->video_dst_data[0],
							pdecinfo->width, pdecinfo->height, 1.0, 5, 5);
			printf("frame_num %d sharpness %d\n", pdecinfo->dec_frame_num, sharpness);
			total_sharpness += sharpness;

			pdecinfo->dec_frame_num++;
		}

		printf("width %d height %d line %d\n", p_input_stream_info->width, p_input_stream_info->height, __LINE__);
		#endif
		pixel_sharpness_val = (float)((float)total_sharpness / pdecinfo->dec_frame_num)/(float)(p_input_stream_info->p_frame->width)/(float)(p_input_stream_info->p_frame->height);
		//printf("total_sharpness %ld pixel_sharpness_val %f dec_frame_num %d\n", 
		//	total_sharpness, pixel_sharpness_val, pdecinfo->dec_frame_num);
		global_frames_of_gop_array[global_decode_gop_num] = pdecinfo->dec_frame_num;
		global_aq_strength_array[global_decode_gop_num]   = get_aq_strength(pixel_sharpness_val);
		global_unsharp_array[global_decode_gop_num++]     = get_unsharp(pixel_sharpness_val);
		pdecinfo->dec_frame_num = 0;
		p_input_stream_info->p_pkt->data += p_input_stream_info->p_pkt->size;
		p_input_stream_info->p_pkt->size = 0;
		//total_gop_num++;

		//memcpy(p_input_stream_info, p_temp_input_stream_info, sizeof(InputStreamInfo));		
	}

NEXT:
	gettimeofday(&before_crf5_part, NULL);

	// convert the yuv to crf5segment
	#if 0
	ret = encode_prepare(p_input_stream_info, pencinfo, pdecinfo, 1, fps);
	if (ret < 0) {
		fprintf(stderr, "Eagle: encode crf 5 prepare fail done\n");
		return ret;
	}

	ret = encode_frame(p_input_stream_info, pencinfo, 5.0, 0, pmeminfo, 0);
	if (ret < 0) {
		fprintf(stderr, "Eagle: encode crf 5 frame fail");
	}
	avcodec_close(pencinfo->codecCtx);
	avcodec_free_context(&(pencinfo->codecCtx));
	av_frame_free(&(pencinfo->frame));
	av_packet_free(&(pencinfo->p_pkt));

	ret = decode_encoded_h264_rawdata(pdec264fmtinfo, pmeminfo, pdecinfo);
	if (ret != 0) {
		fprintf(stderr, "decode crf 5 error fail\n");
		return ret;
	}
	memcpy(pmeminfo->pVideoBufferCrf5, pmeminfo->pDecodeVideoBuffer, FHD_BUFFER_SIZE);
	#else
	memcpy(pmeminfo->pVideoBufferCrf5, pmeminfo->pVideoBuffer, FHD_BUFFER_SIZE);
	#endif

	saved_data_size = 0;
	saved_size = 0;

	gettimeofday(&after_crf5_part, NULL);
	crf5_time_val += 1000000 * (after_crf5_part.tv_sec - before_crf5_part.tv_sec) + (after_crf5_part.tv_usec - before_crf5_part.tv_usec);

	gettimeofday(&before_loop1_part, NULL);

	//check need to use the unsharp or not
	for (int i = 0; i < 10; i++)
	{
		strncpy(pfilterinfoOne->filter_descr + strlen("unsharp=luma_msize_x=5:luma_msize_y=5:luma_amount="), unsharp_val[i], 3);

		ret = unsharp_decoded_yuv(pfilterinfoOne, pmeminfo, p_input_stream_info, fp_filter, 1);
		ret = encode_prepare(p_input_stream_info, pencinfo, pdecinfo, 1, fps);
		if (ret < 0) {
			fprintf(stderr, "Eagle: encode prepare fail in check if use unsharp or not part\n");
			return ret;
		}
		ret = encode_frame(p_input_stream_info, pencinfo, 23.0, 0, pmeminfo, 1);
		if (ret < 0) {
			fprintf(stderr, "Eagle: encode frame fail in check if use unsharp or not part\n");
			return ret;
		}
		avcodec_close(pencinfo->codecCtx);
		avcodec_free_context(&(pencinfo->codecCtx));
		av_frame_free(&(pencinfo->frame));
		av_packet_free(&(pencinfo->p_pkt));
		
		//4. decode the encoded pkt to yuv
		if ((ret = decode_encoded_h264_rawdata(pdec264fmtinfo, pmeminfo, pdecinfo)) != 0){
			fprintf(stderr, "decode error fail\n");
			return ret;
		}
		
		//5. compare the yuv decoded from 4 and 2, to get the target score
		compute_vmaf_prepare(&s, &vmaf_width, &vmaf_height, 
							p_input_stream_info->p_frame->width, p_input_stream_info->p_frame->height, /*1920, 1036,*/ 
							pmeminfo->pVideoBufferCrf5, /*pmeminfo->pVideoBuffer*/pmeminfo->pDecodeVideoBuffer);
		s->stage = 1;
		compute_vmaf(&vmaf_score, fmt, vmaf_width, vmaf_height, read_frame_new, s, model_path, log_file, NULL,
						disable_clip, disable_avx, enable_transform, phone_model, do_psnr, do_ssim, 
						do_ms_ssim, pool_method, n_thread, 5/*n_subsample*/, enable_conf_interval);
		if (vmaf_score < pre_vmaf_score) {
			global_unsharp_array[global_decode_gop_num] = unsharp[i - 1];//(i - 1) / 10.0;
			printf("i %d unsharp %f %f\n", i - 1, global_unsharp_array[global_decode_gop_num], unsharp[i - 1]);
			free(s);
			s = NULL;
			saved_size      = 0;
			saved_data_size = 0;
			break;
		} else if (unsharp[i] > global_unsharp_array[global_decode_gop_num - 1]){
			free(s);
			s = NULL;
			saved_size 		= 0;
			saved_data_size = 0;
			break;
		} else {
			pre_vmaf_score = vmaf_score;
		}
		free(s);
		s = NULL;
		saved_size = 0;
		saved_data_size = 0;
	}
	global_decode_gop_num++;

	printf("global_unsharp_array %f\n", global_unsharp_array[global_decode_gop_num]);

	//strncpy(pfilterinfoOne->filter_descr + strlen("unsharp=luma_msize_x=5:luma_msize_y=5:luma_amount="), global_unsharp_array[global_decode_gop_num], 3);
	printf("filter_descr %s\n", pfilterinfoOne->filter_descr);
	ret = unsharp_decoded_yuv(pfilterinfoOne, pmeminfo, p_input_stream_info, fp_filter, 1);
	//memcpy(pmeminfo->pVideoBuffer1, pmeminfo->pVideoBufferCrf5, FHD_BUFFER_SIZE);

	for (int crf = 18; crf <= 50; crf++) {		
		//3. encode the yuv data decoded in part 2 to h264 file, using crf 18..(FHD start crf = 25)
        if ((ret = encode_prepare(p_input_stream_info, pencinfo, pdecinfo, 1, fps)) < 0) {
            fprintf(stderr, "Eagle: encode prepare fail\n");
            return ret;
        }

		if ((ret = encode_frame(p_input_stream_info, pencinfo, (float)crf, 0, pmeminfo, 1)) < 0) {
    		fprintf(stderr, "Eagle: encode frame fail\n");
    		return ret;
    	}

		avcodec_close(pencinfo->codecCtx);
		avcodec_free_context(&(pencinfo->codecCtx));
		av_frame_free(&(pencinfo->frame));
		av_packet_free(&(pencinfo->p_pkt));
	
    	//4. decode the encoded pkt to yuv
    	if ((ret = decode_encoded_h264_rawdata(pdec264fmtinfo, pmeminfo, pdecinfo)) != 0){
    		fprintf(stderr, "decode error fail\n");
    		return ret;
    	}

    	//5. compare the yuv decoded from 4 and 2, to get the target score
    	compute_vmaf_prepare(&s, &vmaf_width, &vmaf_height, 
    						p_input_stream_info->p_frame->width, p_input_stream_info->p_frame->height, /*1920, 1036,*/ 
    						pmeminfo->pVideoBufferCrf5, /*pmeminfo->pVideoBuffer*/pmeminfo->pDecodeVideoBuffer);
		s->stage = 1;

		compute_vmaf(&vmaf_score, fmt, vmaf_width, vmaf_height, read_frame_new, s, model_path, log_file, NULL,
        				disable_clip, disable_avx, enable_transform, phone_model, do_psnr, do_ssim, 
        				do_ms_ssim, pool_method, n_thread, 1/*n_subsample*/, enable_conf_interval);
		printf("stage 1 vmaf_score %f\n", vmaf_score);
		free(s);
		s = NULL;
		//exit(0);

		av_frame_free(&(pdec264fmtinfo->frame));
		avcodec_close(pdec264fmtinfo->codecCtx);
		av_parser_close(pdec264fmtinfo->pCodecParserCtx);
		avcodec_free_context(&(pdec264fmtinfo->codecCtx));
		free(pdec264fmtinfo->inbuf);
		pdec264fmtinfo->inbuf = NULL;

		stage1_vmaf_score = vmaf_score;
		stage1_bitrate = (float)((float)saved_data_size / 1024) / (float)((float)(DECODE_FRAME_NUM_PER_GOP - 2) / fps) * 8;

		if ((fabs(stage1_vmaf_score - stage1_prev_vmaf_score) <= 1e-6) || stage1_first_flag){
			stage1_first_flag = 0;
			stage1_per_score = 600;
		} else {
			stage1_per_score = (stage1_bitrate - stage1_prev_bitrate) / (stage1_vmaf_score - stage1_prev_vmaf_score);
		}

		printf("stage1_gop %d bitrate %f prev_bitrate %f vmaf_score %f prev_vmaf_score %f crf %d per_score %f saved_data_size %d saved_size %d \n",
				global_stage1_gop_num, stage1_bitrate, stage1_prev_bitrate, stage1_vmaf_score, stage1_prev_vmaf_score, crf, stage1_per_score, saved_data_size, saved_size);
		//exit(1);
		if (stage1_per_score <= target_per_score) {
			printf("stage1_gop %d global_stage1_gop_num stage1_vmaf_score final result %f crf %d stage1_per_score %f stage1_bitrate %f\n", 
					global_stage1_gop_num, stage1_vmaf_score, crf, stage1_per_score, stage1_bitrate);

			stage1_vmaf_score = (stage1_vmaf_score > 96.0) ? 96.0 : ((stage1_vmaf_score < 90.0) ? 90.0 : stage1_vmaf_score);
			printf("vmaf_score %f\n", stage1_vmaf_score);

			global_target_score_array[global_stage1_gop_num] = stage1_vmaf_score;
			stage1_prev_bitrate =
				 stage1_bitrate =
				 stage1_vmaf_score 		=
				 stage1_prev_vmaf_score =
				 stage1_per_score = 0.0;
			//exit(1);
			saved_data_size = 0;
			saved_size = 0;
			break;
		}

		stage1_prev_bitrate = stage1_bitrate;
		stage1_prev_vmaf_score = stage1_vmaf_score;

		saved_data_size = 0;
		saved_size      = 0;
	}
	stage1_first_flag = 1;

	gettimeofday(&after_loop1_part, NULL);
	loop1_time_val += 1000000 * (after_loop1_part.tv_sec - before_loop1_part.tv_sec) + (after_loop1_part.tv_usec - before_loop1_part.tv_usec);

	gop_num++;

    //6. unsharp the decoded yuv data

	gettimeofday(&before_loop2_part, NULL);
	memcpy(pfilterinfo->filter_descr, pfilterinfoOne->filter_descr, 100);
    ret = unsharp_decoded_yuv(pfilterinfo, pmeminfo, p_input_stream_info, fp_filter, 0);
    //memcpy(pmeminfo->pVideoBuffer2, pmeminfo->pVideoBuffer, (FHD_BUFFER_SIZE / 5));
	printf("unsharp_decoded_yuv done\n");
	stage2_last_crf = 18;

	stage2_best_bitrate = 1000000;
	stage2_best_vmaf_diff = 100;
	stage2_best_vmaf  = 0;
	stage2_early_stop = 0;
	stage2_loop_count = 0;
	stage2_first_flag = 1;
	stage2_crf_step   = 1.0;
	stage2_start_crf  = 18;

	//if (global_target_score_array[global_stage2_gop_num] - 2 >= 91) {
	//	stage2_target_vmaf_score = global_target_score_array[global_stage2_gop_num] - 2;
	//} else {
	//	stage2_target_vmaf_score = global_target_score_array[global_stage2_gop_num];
	//}

	for (int crf = 18; crf < 40; /*crf++*/) {
		stage2_crf_step = 1;

    	//7. encode the filtered frame to get the crf
		ret = enc_filtered_yuv_to_264(pmeminfo, (float)(crf), p_input_stream_info, fps);

    	//8. decode the encoded 264 raw data to yuv
    	ret = decode_filtered_encoded_h264_rawdata(pmeminfo, pdecinfo);

		if (0)
		{
			FILE *fp = fopen("./pVideoBuffer.yuv", "wb");
			fwrite(pmeminfo->pVideoBuffer, 1, FHD_BUFFER_SIZE / 5, fp);
			fclose(fp);
			fp = fopen("./pDecodeVideoBuffer2.yuv", "wb");
			fwrite(pmeminfo->pDecodeVideoBuffer2, 1, FHD_BUFFER_SIZE / 5, fp);
			fclose(fp);
		}

		//9. compute the vmaf and determine the finally crf value
		printf("width %d height %d linesize %d %d\n", 
				p_input_stream_info->p_frame->width, p_input_stream_info->p_frame->height,
				p_input_stream_info->p_frame->linesize[0], p_input_stream_info->p_frame->linesize[1]);
    	compute_vmaf_prepare(&s, &vmaf_width, &vmaf_height, 
    						p_input_stream_info->p_frame->width, p_input_stream_info->p_frame->height, 
    						pmeminfo->pVideoBuffer, pmeminfo->pDecodeVideoBuffer2);

		s->stage      = 2;
		s->num_frames = 5;

		compute_vmaf(&vmaf_score, fmt, vmaf_width, vmaf_height, read_frame_new, s, model_path, log_file_2, NULL,
        				disable_clip, disable_avx, enable_transform, phone_model, do_psnr, do_ssim, 
        				do_ms_ssim, pool_method, n_thread, n_subsample, enable_conf_interval);
		printf("stage 2 vmaf_score %f\n", vmaf_score);

		stage2_score_in   = vmaf_score;
		stage2_bitrate_in = (float)((float)(saved_data_size_filtered/* -  enc_pkt_size_filtered[5]*/)/ 1024) / (float)((float)(FILTERED_FRAME_NUM_PER_GOP - 4) / fps) * 8;
		stage2_vmaf_diff  = stage2_score_in - stage2_target_vmaf_score;

		if (stage2_bitrate_in > stage2_best_bitrate && abs(stage2_vmaf_diff) < 5 && 0){
			//early_stop never happen
		}

		//best descition
		if ((stage2_vmaf_diff > -1 && stage2_bitrate_in < stage2_best_bitrate) || stage2_first_flag) {
			stage2_best_bitrate   = stage2_bitrate_in;
			stage2_best_vmaf_diff = stage2_vmaf_diff;
			stage2_best_vmaf      = stage2_score_in;
			stage2_best_crf       = crf;
			stage2_first_flag     = 0;
		}

		if (abs(stage2_vmaf_diff) < 1 && stage2_vmaf_diff < 0.2) {
			printf("stage2_vmaf_diff %f crf %d line %d\n", stage2_vmaf_diff, crf, __LINE__);
			saved_data_size_filtered = 0;
			global_crf_array[global_stage2_gop_num] = (int)crf + 1;
			stage2_last_crf = crf;
			free(s);
			//exit(0);
			s = NULL;
			break;
		}

		//crf descition
		stage2_step_vmaf_res = (float)((crf - 18) / 10.0);
		stage2_step_vmaf_res = (stage2_step_vmaf_res < 0.2) ? 0.2 : stage2_step_vmaf_res;

		
		if (stage2_vmaf_diff > 20.0)
			stage2_step_vmaf = 1.5 * stage2_step_vmaf_res;
		else if (stage2_vmaf_diff > 15)
			stage2_step_vmaf = 2 * stage2_step_vmaf_res;
		else if (stage2_vmaf_diff > 10)
			stage2_step_vmaf = 2.5 * stage2_step_vmaf_res;
		else
			stage2_step_vmaf = 4 * stage2_step_vmaf_res;

		stage2_step_vmaf = (stage2_step_vmaf < 1) ? 1 : stage2_step_vmaf;

		stage2_score_diff = stage2_score_in - stage2_target_vmaf_score;
		printf("stage2_step_vmaf stage2_score_diff target_score stage2_score_in %f %f %f %f\n",
				stage2_step_vmaf, stage2_score_diff, stage2_target_vmaf_score, stage2_score_in);

		if (stage2_score_diff > 0) {
			stage2_crf_step = stage2_score_diff / stage2_step_vmaf;
			stage2_crf_step = (stage2_crf_step < 1) ? 1 : stage2_crf_step;
			if (crf < (int)stage2_last_crf){
				//getchar();
				saved_data_size_filtered = 0;
				global_crf_array[global_stage2_gop_num] = (int)crf + 1;
				stage2_last_crf = crf;
				printf("global_stage2_gop_num %d global_crf_array[%d] %f\n", global_stage2_gop_num, global_stage2_gop_num, global_crf_array[global_stage2_gop_num]);
				free(s);
				s = NULL;
				break;
			}
		} else {
			if ((crf == (int)(stage2_last_crf + 1)) || (crf == (int)(stage2_start_crf)) || (crf == 18) || (crf == (int)(stage2_last_crf - 1))) {
				saved_data_size_filtered = 0;
				global_crf_array[global_stage2_gop_num] = (int)crf + 1;
				printf("stage2_last_crf %d \n", stage2_last_crf);
				stage2_last_crf = crf;
				printf("global_stage2_gop_num %d global_crf_array[%d] %f\n", global_stage2_gop_num, global_stage2_gop_num, global_crf_array[global_stage2_gop_num]);
				free(s);
				//exit(0);
				s = NULL;
				break;
			} else {
				stage2_crf_step = stage2_score_diff / stage2_step_vmaf;
				stage2_crf_step = (stage2_crf_step > -1) ? -1 : stage2_crf_step;
			}
		}

		if (stage2_crf_step > 5)
			stage2_crf_step = 5;
		if (stage2_crf_step < -2)
			stage2_crf_step = -2;
		stage2_crf_step = (stage2_crf_step < 0) ? (int)(stage2_crf_step) : stage2_crf_step;

		stage2_last_crf = crf;

		printf("stage2_crf_step %f\n", stage2_crf_step);
		if (crf + stage2_crf_step > 40) {
			saved_data_size_filtered = 0;
			global_crf_array[global_stage2_gop_num] = (int)(crf) + 1;
			free(s);
			s = NULL;
			break;
		} else {
			crf = (int)(crf + stage2_crf_step);
		}

		if (vmaf_score < stage2_target_vmaf_score) {
			printf("global_stage2_gop_num %d crf %d vmaf_score %f global_crf_array[%d] %f\n",
				global_stage2_gop_num, crf, vmaf_score, global_stage2_gop_num, global_crf_array[global_stage2_gop_num]);
			free(s);
			s = NULL;
			saved_data_size_filtered = 0;
			global_crf_array[global_stage2_gop_num] = (int)crf + 1;
			break;
		}

		free(s);
		s = NULL;
		saved_data_size_filtered = 0;
	}
	printf("after one gop target_score %f global_crf_array[%d] %f\n", 
		stage2_target_vmaf_score, global_stage2_gop_num, global_crf_array[global_stage2_gop_num]);
	global_stage1_gop_num++;
	global_stage2_gop_num++;
	//exit(0);
	gettimeofday(&after_loop2_part, NULL);
	loop2_time_val += 1000000 * (after_loop2_part.tv_sec - before_loop2_part.tv_sec) + (after_loop2_part.tv_usec - before_loop2_part.tv_usec);
	printf("loop2_time_val %lld\n", loop2_time_val);

	printf("Statistics Time crf5_time_val %lld loop1_time_val %lld loop2_time_val %lld\n", 
			crf5_time_val, loop1_time_val, loop2_time_val);

	if (end_of_file) {
		free(pmeminfo->pVideoBuffer); 		pmeminfo->pVideoBuffer 		  = NULL;
		free(pmeminfo->pVideoBuffer2); 		pmeminfo->pVideoBuffer2 	  = NULL;
		free(pmeminfo->pEncodeVideoBuffer); pmeminfo->pEncodeVideoBuffer  = NULL;
		free(pmeminfo->pEncodeVideoBuffer2);pmeminfo->pEncodeVideoBuffer2 = NULL;
		free(pmeminfo->pDecodeVideoBuffer);	pmeminfo->pDecodeVideoBuffer  = NULL;
		free(pmeminfo->pDecodeVideoBuffer2);pmeminfo->pDecodeVideoBuffer2 = NULL;
		free(pmeminfo);				pmeminfo 	= NULL;
		av_freep(&(pdecinfo->video_dst_data[0]));
		free(pdecinfo);				pdecinfo	= NULL;
		free(pencinfo);				pencinfo	= NULL;
		if (pdec264fmtinfo->outputfp)
			fclose(pdec264fmtinfo->outputfp);
		free(pdec264fmtinfo);		pdec264fmtinfo	= NULL;
		free(pfilterinfo);			pfilterinfo		= NULL;
		av_packet_free(&(p_input_stream_info->p_pkt));  p_input_stream_info->p_pkt   = NULL;
		av_frame_free(&(p_input_stream_info->p_frame)); p_input_stream_info->p_frame = NULL;
		avformat_close_input(&(p_input_stream_info->p_fmt_ctx));
		avcodec_free_context(&(p_input_stream_info->p_video_codecctx));
		free(p_input_stream_info);	p_input_stream_info = NULL;
		if (inputpar.video_dst_file)
			fclose(inputpar.video_dst_file);
		if (fp_filter)
			fclose(fp_filter);
		return ret;
	}
	else
		goto DECODE_ORG_BITS;

	return ret;
}

static int EagleParseParam(int argc, char **argv)
{
	int ret_arg = 0;
	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "-i"))         {ret_arg = i + 1;i++;}
		if (!strcmp(argv[i], "-preset"))    {argv[i + 1] = "medium";  i++;}
		if (!strcmp(argv[i], "-tune"))      {argv[i + 1] = "ssim";    i++;}
		if (!strcmp(argv[i], "-profile:v")) {argv[i + 1] = "high";    i++;}
		if (!strcmp(argv[i], "-c:v"))		{argv[i + 1]  = "libx264"; i++;}
		if (!strcmp(argv[i], "-b:v"))		{printf("cannot set the bitrate param\n");exit(1);}
	}

	return ret_arg;
}

int main(int argc, char **argv)
{
    int i, ret;
	int eagle_argc = 0;
    BenchmarkTimeStamps ti;
	struct timeval start, end;
	gettimeofday(&start, NULL);
	//TODO:The first process is to get the target_vmaf and sharpness value for each segment
	ret = EagleParseParam(argc, argv);
	char **eagle_argv = (char *)malloc((argc + 10) * (sizeof(int)) * sizeof(char *));
	for (int i = 0; i < argc - 1; i++) {
		eagle_argv[i] = (char *)malloc(strlen(argv[i]) + 1);
		memcpy(eagle_argv[i], argv[i], strlen(argv[i]) + 1);
	}

	#if 1
	eagle_argv[argc - 1] = (char *)malloc(strlen("-vf") + 1);
	memcpy(eagle_argv[argc - 1], "-vf", strlen("-vf") + 1);
	eagle_argv[argc - 1 + 1] = (char *)malloc(strlen("unsharp=5:5:1.0") + 1);
	memcpy(eagle_argv[argc - 1 + 1], "unsharp=5:5:1.0", strlen("unsharp=5:5:1.0") + 1);

	eagle_argv[argc - 1 + 2] = (char *)malloc(strlen("-c:v") + 1);
	memcpy(eagle_argv[argc - 1 + 2], "-c:v", strlen("-c:v") + 1);
	eagle_argv[argc - 1 + 3] = (char *)malloc(strlen("libx264") + 1);
	memcpy(eagle_argv[argc - 1 + 3], "libx264", strlen("libx264") + 1);

	eagle_argv[argc - 1 + 4] = (char *)malloc(strlen("-profile:v") + 1);
	memcpy(eagle_argv[argc - 1 + 4], "-profile:v", strlen("-profile:v") + 1);
	eagle_argv[argc - 1 + 5] = (char *)malloc(strlen("high") + 1);
	memcpy(eagle_argv[argc - 1 + 5], "high", strlen("high") + 1);

	eagle_argv[argc - 1 + 6] = (char *)malloc(strlen("-preset") + 1);
	memcpy(eagle_argv[argc - 1 + 6], "-preset", strlen("-preset") + 1);
	eagle_argv[argc - 1 + 7] = (char *)malloc(strlen("medium") + 1);
	memcpy(eagle_argv[argc - 1 + 7], "medium", strlen("medium") + 1);

	eagle_argv[argc - 1 + 8] = (char *)malloc(strlen("-tune") + 1);
	memcpy(eagle_argv[argc - 1 + 8], "-tune", strlen("-tune") + 1);
	eagle_argv[argc - 1 + 9] = (char *)malloc(strlen("ssim") + 1);
	memcpy(eagle_argv[argc - 1 + 9], "ssim", strlen("ssim") + 1);

	eagle_argv[argc - 1 + 10] = (char *)malloc(strlen(argv[argc - 1]) + 1);
	memcpy(eagle_argv[argc - 1 + 10], argv[argc - 1], strlen(argv[argc - 1]) + 1);

	eagle_argc = argc + 10;
	#else
	eagle_argv[argc - 1] = (char *)malloc(strlen("-c:v") + 1);
	memcpy(eagle_argv[argc - 1], "-c:v", strlen("-c:v") + 1);
	eagle_argv[argc - 1 + 1] = (char *)malloc(strlen("libx264") + 1);
	memcpy(eagle_argv[argc - 1 + 1], "libx264", strlen("libx264") + 1);

	eagle_argv[argc - 1 + 2] = (char *)malloc(strlen("-profile:v") + 1);
	memcpy(eagle_argv[argc - 1 + 2], "-profile:v", strlen("-profile:v") + 1);
	eagle_argv[argc - 1 + 3] = (char *)malloc(strlen("high") + 1);
	memcpy(eagle_argv[argc - 1 + 3], "high", strlen("high") + 1);

	eagle_argv[argc - 1 + 4] = (char *)malloc(strlen("-preset") + 1);
	memcpy(eagle_argv[argc - 1 + 4], "-preset", strlen("-preset") + 1);
	eagle_argv[argc - 1 + 5] = (char *)malloc(strlen("medium") + 1);
	memcpy(eagle_argv[argc - 1 + 5], "medium", strlen("medium") + 1);

	eagle_argv[argc - 1 + 6] = (char *)malloc(strlen("-tune") + 1);
	memcpy(eagle_argv[argc - 1 + 6], "-tune", strlen("-tune") + 1);
	eagle_argv[argc - 1 + 7] = (char *)malloc(strlen("ssim") + 1);
	memcpy(eagle_argv[argc - 1 + 7], "ssim", strlen("ssim") + 1);

	eagle_argv[argc - 1 + 8] = (char *)malloc(strlen(argv[argc - 1]) + 1);
	memcpy(eagle_argv[argc - 1 + 8], argv[argc - 1], strlen(argv[argc - 1]) + 1);

	eagle_argc = argc + 8;

	#endif
	for (int i = 0; i < eagle_argc; i++){
		printf("eagle_argc[%d] %s\n", i, eagle_argv[i]);
	}

	//exit(0);
    EaglePreProcess(argv[ret]);

	gettimeofday(&end, NULL);
	printf("interval = %ld\n", 1000000*(end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec));

    init_dynload();

    register_exit(ffmpeg_cleanup);

    setvbuf(stderr,NULL,_IONBF,0); /* win32 runtime needs this */

    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    parse_loglevel(eagle_argc, eagle_argv, options);

    if(eagle_argc>1 && !strcmp(eagle_argv[1], "-d")){
        run_as_daemon=1;
        av_log_set_callback(log_callback_null);
        eagle_argc--;
        eagle_argv++;
    }

#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
    avformat_network_init();

    show_banner(eagle_argc, eagle_argv, options);

    /* parse options and open all input/output files */
    ret = ffmpeg_parse_options(eagle_argc, eagle_argv);
    if (ret < 0)
        exit_program(1);

    if (nb_output_files <= 0 && nb_input_files == 0) {
        show_usage();
        av_log(NULL, AV_LOG_WARNING, "Use -h to get full help or, even better, run 'man %s'\n", program_name);
        exit_program(1);
    }

    /* file converter / grab */
    if (nb_output_files <= 0) {
        av_log(NULL, AV_LOG_FATAL, "At least one output file must be specified\n");
        exit_program(1);
    }

    for (i = 0; i < nb_output_files; i++) {
        if (strcmp(output_files[i]->ctx->oformat->name, "rtp"))
            want_sdp = 0;
    }

    current_time = ti = get_benchmark_time_stamps();
    if (transcode() < 0)
        exit_program(1);

    if (do_benchmark) {
        int64_t utime, stime, rtime;
        current_time = get_benchmark_time_stamps();
        utime = current_time.user_usec - ti.user_usec;
        stime = current_time.sys_usec  - ti.sys_usec;
        rtime = current_time.real_usec - ti.real_usec;
        av_log(NULL, AV_LOG_INFO,
               "bench: utime=%0.3fs stime=%0.3fs rtime=%0.3fs\n",
               utime / 1000000.0, stime / 1000000.0, rtime / 1000000.0);
    }
    av_log(NULL, AV_LOG_DEBUG, "%"PRIu64" frames successfully decoded, %"PRIu64" decoding errors\n",
           decode_error_stat[0], decode_error_stat[1]);
    if ((decode_error_stat[0] + decode_error_stat[1]) * max_error_rate < decode_error_stat[1])
        exit_program(69);

	gettimeofday(&end, NULL);
	printf("interval = %ld\n", 1000000*(end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec));
    exit_program(received_nb_signals ? 255 : main_return_code);
	gettimeofday(&end, NULL);
	printf("interval = %ld\n", 1000000*(end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec));

	return main_return_code;
}
