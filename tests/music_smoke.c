/* Generate our own Vorbis tone and test codec decoding plus WinMM CD-audio metadata. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <vorbis/vorbisenc.h>
#include <vorbis/vorbisfile.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void write_page(FILE *output, ogg_page *page)
{
    if (fwrite(page->header, 1, page->header_len, output) != (size_t)page->header_len ||
        fwrite(page->body, 1, page->body_len, output) != (size_t)page->body_len) exit(20);
}

static void generate_tone(const char *path)
{
    vorbis_info information;
    vorbis_comment comments;
    vorbis_dsp_state state;
    vorbis_block block;
    ogg_stream_state stream;
    ogg_packet header, comment, codebook, packet;
    ogg_page page;
    FILE *output = fopen(path, "wb");
    float **samples;
    if (!output) exit(21);
    vorbis_info_init(&information);
    if (vorbis_encode_init_vbr(&information, 2, 44100, 0.2f)) exit(22);
    vorbis_comment_init(&comments);
    vorbis_comment_add_tag(&comments, "ENCODER", "Imperialism Compat generated test tone");
    if (vorbis_analysis_init(&state, &information) || vorbis_block_init(&state, &block) ||
        ogg_stream_init(&stream, 12345)) exit(23);
    vorbis_analysis_headerout(&state, &comments, &header, &comment, &codebook);
    ogg_stream_packetin(&stream, &header);
    ogg_stream_packetin(&stream, &comment);
    ogg_stream_packetin(&stream, &codebook);
    while (ogg_stream_flush(&stream, &page)) write_page(output, &page);
    samples = vorbis_analysis_buffer(&state, 11025);
    for (int index = 0; index < 11025; index++)
        samples[0][index] = samples[1][index] = (float)(0.1 * sin(index * 6.283185307179586 * 440 / 44100));
    vorbis_analysis_wrote(&state, 11025);
    vorbis_analysis_wrote(&state, 0);
    while (vorbis_analysis_blockout(&state, &block) == 1) {
        vorbis_analysis(&block, NULL);
        vorbis_bitrate_addblock(&block);
        while (vorbis_bitrate_flushpacket(&state, &packet)) {
            ogg_stream_packetin(&stream, &packet);
            while (ogg_stream_pageout(&stream, &page)) write_page(output, &page);
        }
    }
    fclose(output);
    ogg_stream_clear(&stream);
    vorbis_block_clear(&block);
    vorbis_dsp_clear(&state);
    vorbis_comment_clear(&comments);
    vorbis_info_clear(&information);
}

int main(int argc, char **argv)
{
    typedef MCIERROR (WINAPI *SendString)(LPCSTR, LPSTR, UINT, HWND);
    HMODULE module;
    SendString send;
    OggVorbis_File file;
    char buffer[4096];
    int section;
    long amount;
    size_t decoded = 0;
    if (argc != 3) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    generate_tone(argv[2]);
    if (ov_fopen(argv[2], &file)) return 3;
    if (ov_info(&file, -1)->channels != 2 || ov_time_total(&file, -1) < 0.24) return 4;
    while ((amount = ov_read(&file, buffer, sizeof buffer, 0, 2, 1, &section)) > 0) decoded += amount;
    ov_clear(&file);
    if (amount < 0 || decoded < 40000) return 5;
    module = LoadLibraryA(argv[1]);
    if (!module) { fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError()); return 6; }
    send = (SendString)GetProcAddress(module, "mciSendStringA");
    if (!send) return 7;
    if (send("open d: type cdaudio alias compat", NULL, 0, NULL)) return 8;
    /* CD track 1 is the data track; the first generated music track is number 2. */
    if (send("status compat number of tracks", buffer, sizeof buffer, NULL) || atoi(buffer) != 2) return 9;
    if (send("set compat time format milliseconds", NULL, 0, NULL)) return 10;
    if (send("status compat length track 2", buffer, sizeof buffer, NULL) || atoi(buffer) < 240) return 11;
    if (send("close compat", NULL, 0, NULL)) return 12;
    /* The wrapper owns a process-lifetime worker; exercise process exit, not hot unloading. */
    printf("decoded %zu PCM bytes; CD-audio open/status/length/close passed\n", decoded);
    return 0;
}
