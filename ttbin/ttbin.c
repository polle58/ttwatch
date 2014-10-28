/*****************************************************************************\
** ttbin.c                                                                   **
** TTBIN parsing implementation                                              **
\*****************************************************************************/

#include "ttbin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

/*****************************************************************************/

#define TAG_FILE_HEADER     (0x20)
#define TAG_STATUS          (0x21)
#define TAG_GPS             (0x22)
#define TAG_HEART_RATE      (0x25)
#define TAG_SUMMARY         (0x27)
#define TAG_LAP             (0x2f)
#define TAG_TREADMILL       (0x32)
#define TAG_SWIM            (0x34)

typedef struct __attribute__((packed))
{
    uint8_t tag;
    uint16_t length;
} RECORD_LENGTH;

typedef struct __attribute__((packed))
{
    uint8_t  file_version;
    uint8_t  firmware_version[4];
    uint16_t product_id;
    uint32_t timestamp;     /* local time */
    uint8_t  _unk[105];
    uint8_t  length_count;  /* number of RECORD_LENGTH objects to follow */
} FILE_HEADER;

typedef struct __attribute__((packed))
{
    uint8_t  activity;
    float    distance;
    uint32_t duration;      /* seconds, after adding 1 */
    uint16_t calories;
} FILE_SUMMARY_RECORD;

typedef struct __attribute__((packed))
{
    int32_t  latitude;      /* degrees * 1e7 */
    int32_t  longitude;     /* degrees * 1e7 */
    uint16_t heading;       /* degrees * 100, N = 0, E = 9000 */
    uint16_t speed;         /* m/s * 100 */
    uint32_t timestamp;     /* gps time (utc) */
    uint16_t calories;
    float    inc_distance;  /* metres */
    float    cum_distance;  /* metres */
    uint8_t  cycles;        /* steps/strokes/cycles etc. */
} FILE_GPS_RECORD;

typedef struct __attribute__((packed))
{
    uint8_t  heart_rate;    /* bpm */
    uint8_t  _unk;
    uint32_t timestamp;     /* local time */
} FILE_HEART_RATE_RECORD;

typedef struct __attribute__((packed))
{
    uint8_t  status;        /* 0 = ready, 1 = active, 2 = paused, 3 = stopped */
    uint8_t  activity;      /* 0 = running, 1 = cycling, 2 = swimming, 7 = treadmill, 8 = freestyle */
    uint32_t timestamp;     /* local time */
} FILE_STATUS_RECORD;

typedef struct __attribute__((packed))
{
    uint32_t timestamp;     /* local time */
    float    distance;      /* metres */
    uint16_t calories;
    uint32_t steps;
    uint16_t _unk;
} FILE_TREADMILL_RECORD;

typedef struct __attribute__((packed))
{
    uint32_t timestamp;         /* local time */
    float    total_distance;    /* metres */
    uint8_t  _unk1;             /* always 0xff */
    uint8_t  _unk2;
    uint32_t strokes;           /* since the last report */
    uint32_t completed_laps;
    uint16_t total_calories;
} FILE_SWIM_RECORD;

typedef struct __attribute__((packed))
{
    uint32_t total_time;        /* seconds since activity start */
    float    total_distance;    /* metres */
    uint16_t total_calories;
} FILE_LAP_RECORD;

/*****************************************************************************/

TTBIN_FILE *read_ttbin_file(FILE *file)
{
    uint32_t size = 0;
    uint8_t *data = 0;
    TTBIN_FILE *ttbin;

    while (!feof(file))
    {
        data = realloc(data, size + 1024);
        size += fread(data + size, 1, 1024, file);
    }

    ttbin = parse_ttbin_data(data, size);

    free(data);
    return ttbin;
}


/*****************************************************************************/

TTBIN_FILE *parse_ttbin_data(uint8_t *data, uint32_t size)
{
    uint8_t *end;
    RECORD_LENGTH record_lengths[24] = {0};
    TTBIN_FILE *file;
    uint32_t initial_gps_time = 0;
    uint32_t initial_hr_time = 0;
    uint32_t initial_treadmill_time = 0;
    uint32_t initial_swim_time = 0;
    int index;

    FILE_HEADER             file_header;
    FILE_SUMMARY_RECORD     summary_record;
    FILE_GPS_RECORD         gps_record;
    FILE_HEART_RATE_RECORD  heart_rate_record;
    FILE_STATUS_RECORD      status_record;
    FILE_TREADMILL_RECORD   treadmill_record;
    FILE_SWIM_RECORD        swim_record;
    FILE_LAP_RECORD         lap_record;

    file = malloc(sizeof(TTBIN_FILE));
    memset(file, 0, sizeof(*file));

    end = data + size;

    while (data < end)
    {
        uint8_t tag = *data++;
        switch (tag)
        {
        case TAG_FILE_HEADER:
            memcpy(&file_header, data, sizeof(FILE_HEADER));
            data += sizeof(FILE_HEADER);
            memcpy(record_lengths, data, file_header.length_count * sizeof(RECORD_LENGTH));
            data += file_header.length_count * sizeof(RECORD_LENGTH);

            file->file_version = file_header.file_version;
            memcpy(file->firmware_version, file_header.firmware_version, sizeof(file->firmware_version));
            file->product_id = file_header.product_id;
            file->timestamp  = file_header.timestamp;
            break;
        case TAG_SUMMARY:
            memcpy(&summary_record, data, sizeof(FILE_SUMMARY_RECORD));

            file->activity = summary_record.activity;
            file->total_distance = summary_record.distance;
            file->duration = summary_record.duration;
            file->total_calories = summary_record.calories;
            break;
        case TAG_STATUS:
            memcpy(&status_record, data, sizeof(FILE_STATUS_RECORD));
            file->status_records = realloc(file->status_records, (file->status_record_count + 1) * sizeof(STATUS_RECORD));

            file->status_records[file->status_record_count].status    = status_record.status;
            file->status_records[file->status_record_count].activity  = status_record.activity;
            file->status_records[file->status_record_count].timestamp = status_record.timestamp;
            ++file->status_record_count;
            break;
        case TAG_GPS:
            memcpy(&gps_record, data, sizeof(FILE_GPS_RECORD));

            if (initial_gps_time == 0)
                initial_gps_time = gps_record.timestamp;

            /* if the GPS signal is lost, 0xffffffff is stored in the file */
            if (gps_record.timestamp == 0xffffffff)
                break;

            index = gps_record.timestamp - initial_gps_time;

            /* expand the array if necessary */
            if (index >= file->gps_record_count)
            {
                file->gps_records = realloc(file->gps_records, (index + 1) * sizeof(GPS_RECORD));
                memset(file->gps_records + file->gps_record_count, 0, (index + 1 - file->gps_record_count) * sizeof(GPS_RECORD));
                file->gps_record_count = index + 1;
            }

            file->gps_records[index].latitude     = gps_record.latitude * 1e-7f;
            file->gps_records[index].longitude    = gps_record.longitude * 1e-7f;
            file->gps_records[index].elevation    = 0.0f;
            file->gps_records[index].heading      = gps_record.heading / 100.0f;
            file->gps_records[index].speed        = gps_record.speed / 100.0f;
            file->gps_records[index].timestamp    = gps_record.timestamp;
            file->gps_records[index].calories     = gps_record.calories;
            file->gps_records[index].inc_distance = gps_record.inc_distance;
            file->gps_records[index].cum_distance = gps_record.cum_distance;
            file->gps_records[index].cycles       = gps_record.cycles;
            break;
        case TAG_HEART_RATE:
            memcpy(&heart_rate_record, data, sizeof(FILE_HEART_RATE_RECORD));

            file->has_heart_rate = 1;

            if (initial_hr_time == 0)
                initial_hr_time = heart_rate_record.timestamp;

            index = heart_rate_record.timestamp - initial_hr_time;

            if (file->gps_records)
            {
                /* expand the array if necessary */
                if (index >= file->gps_record_count)
                {
                    file->gps_records = realloc(file->gps_records, (index + 1) * sizeof(GPS_RECORD));
                    memset(file->gps_records + file->gps_record_count, 0, (index + 1 - file->gps_record_count) * sizeof(GPS_RECORD));
                    file->gps_record_count = index + 1;
                }

                file->gps_records[index].timestamp  = initial_gps_time + index;
                file->gps_records[index].heart_rate = heart_rate_record.heart_rate;
            }
            else if (file->treadmill_records)
            {
                /* expand the array if necessary */
                if (index >= file->treadmill_record_count)
                {
                    file->treadmill_records = realloc(file->treadmill_records, (index + 1) * sizeof(TREADMILL_RECORD));
                    memset(file->gps_records + file->treadmill_record_count, 0,
                        (index + 1 - file->treadmill_record_count) * sizeof(TREADMILL_RECORD));
                    file->treadmill_record_count = index + 1;
                }

                file->treadmill_records[index].timestamp = initial_treadmill_time + index;
                file->treadmill_records[index].heart_rate = heart_rate_record.heart_rate;
            }
            break;
        case TAG_LAP:
            memcpy(&lap_record, data, sizeof(FILE_LAP_RECORD));
            file->lap_records = realloc(file->lap_records, (file->lap_record_count + 1) * sizeof(LAP_RECORD));

            file->lap_records[file->lap_record_count].total_time     = lap_record.total_time;
            file->lap_records[file->lap_record_count].total_distance = lap_record.total_distance;
            file->lap_records[file->lap_record_count].total_calories = lap_record.total_calories;
            ++file->lap_record_count;
            break;
        case TAG_TREADMILL:
            memcpy(&treadmill_record, data, sizeof(FILE_TREADMILL_RECORD));

            if (initial_treadmill_time == 0)
                initial_treadmill_time = treadmill_record.timestamp;

            index = treadmill_record.timestamp - initial_treadmill_time;

            /* expand the array if necessary */
            if (index >= file->treadmill_record_count)
            {
                file->treadmill_records = realloc(file->treadmill_records, (index + 1) * sizeof(TREADMILL_RECORD));
                memset(file->treadmill_records + file->treadmill_record_count, 0,
                    (index + 1 - file->treadmill_record_count) * sizeof(TREADMILL_RECORD));
                file->treadmill_record_count = index + 1;
            }

            file->treadmill_records[index].timestamp = treadmill_record.timestamp;
            file->treadmill_records[index].distance  = treadmill_record.distance;
            file->treadmill_records[index].calories  = treadmill_record.calories;
            file->treadmill_records[index].steps     = treadmill_record.steps;
            break;
        case TAG_SWIM:
            memcpy(&swim_record, data, sizeof(FILE_SWIM_RECORD));

            if (initial_swim_time == 0)
                initial_swim_time = swim_record.timestamp;

            index = swim_record.timestamp - initial_swim_time;

            /* expand the array if necessary */
            if (index >= file->swim_record_count)
            {
                file->swim_records = realloc(file->swim_records, (index + 1) * sizeof(TREADMILL_RECORD));
                memset(file->gps_records + file->swim_record_count, 0,
                    (index + 1 - file->swim_record_count) * sizeof(TREADMILL_RECORD));
                file->swim_record_count = index + 1;
            }

            file->swim_records[index].timestamp      = swim_record.timestamp;
            file->swim_records[index].total_distance = swim_record.total_distance;
            file->swim_records[index].strokes        = swim_record.strokes;
            file->swim_records[index].completed_laps = swim_record.completed_laps;
            file->swim_records[index].total_calories = swim_record.total_calories;
            break;
        default:
            break;
        }

        /* increment the data by the correct amount */
        if (tag != TAG_FILE_HEADER)
        {
            RECORD_LENGTH *entry = record_lengths;
            while ((entry->tag != tag) && (entry->tag != 0))
                entry++;
            if (entry->tag == tag)
                data += entry->length - 1;
        }
    }

    return file;
}

/*****************************************************************************/

const char *create_filename(TTBIN_FILE *ttbin, const char *ext)
{
    static char filename[256];
    struct tm *time = gmtime(&ttbin->timestamp);

    switch (ttbin->activity)
    {
    case ACTIVITY_RUNNING:
        sprintf(filename, "Running_%02d-%02d-%02d.%s", time->tm_hour, time->tm_min, time->tm_sec, ext);
        break;
    case ACTIVITY_CYCLING:
        sprintf(filename, "Cycling_%02d-%02d-%02d.%s", time->tm_hour, time->tm_min, time->tm_sec, ext);
        break;
    case ACTIVITY_SWIMMING:
        sprintf(filename, "Pool_swim_%02d-%02d-%02d.%s", time->tm_hour, time->tm_min, time->tm_sec, ext);
        break;
    case ACTIVITY_TREADMILL:
        sprintf(filename, "Treadmill_%02d-%02d-%02d.%s", time->tm_hour, time->tm_min, time->tm_sec, ext);
        break;
    case ACTIVITY_FREESTYLE:
        sprintf(filename, "Freestyle_%02d-%02d-%02d.%s", time->tm_hour, time->tm_min, time->tm_sec, ext);
        break;
    default:
        sprintf(filename, "Unknown_%02d-%02d-%02d.%s", time->tm_hour, time->tm_min, time->tm_sec, ext);
        break;
    }

    return filename;
}

/*****************************************************************************/

typedef struct
{
    GPS_RECORD *data;
    uint32_t max_count;
    uint32_t current_count;

    float elev;
    float mult;
} ELEV_DATA_INFO;

static size_t curl_write_data(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    ELEV_DATA_INFO *info = (ELEV_DATA_INFO*)userdata;
    char *s1;

    size_t length = size * nmemb;

    /* this is a simple float-parser that maintains state between
       invocations incase we get a single number split between
       multiple buffers */
    for (s1 = ptr; s1 < (ptr + length); ++s1)
    {
        if (isdigit(*s1))
        {
            if (info->mult > 0.5f)
                info->elev = (info->elev * 10.0f) + (*s1 - '0');
            else
            {
                info->elev += info->mult * (*s1 - '0');
                info->mult /= 10.0f;
            }
        }
        else if (*s1 == '.')
            info->mult = 0.1f;
        else if ((*s1 == ',') || (*s1 == ']'))
        {
            if (info->current_count < info->max_count)
            {
                info->data->elevation = info->elev;
                ++info->current_count;
                ++info->data;
            }
            info->elev = 0.0f;
            info->mult = 1.0f;
        }
    }

    return length;
}

void download_elevation_data(TTBIN_FILE *ttbin)
{
    CURL *curl;
    struct curl_slist *headers;
    char *post_data;
    char *response_data;
    char *str;
    uint32_t i;
    ELEV_DATA_INFO info = {0};
    int result;

    curl = curl_easy_init();
    if (!curl)
    {
        fprintf(stderr, "Unable to initialise libcurl\n");
        return;
    }

    /* create the post string to send to the server */
    post_data = malloc(ttbin->gps_record_count * 52 + 10);
    str = post_data;
    str += sprintf(str, "[\n");
    for (i = 0; i < ttbin->gps_record_count; ++i)
    {
        if (i != (ttbin->gps_record_count - 1))
        {
            str += sprintf(str, "   [ %f, %f ],\n",
                ttbin->gps_records[i].latitude,
                ttbin->gps_records[i].longitude);
        }
        else
        {
            str += sprintf(str, "   [ %f, %f ]\n",
                ttbin->gps_records[i].latitude,
                ttbin->gps_records[i].longitude);
        }
    }
    str += sprintf(str, "]\n");

    headers = curl_slist_append(NULL, "Content-Type:text/plain");

    /* setup the callback function data structure */
    info.mult = 1.0;
    info.elev = 0.0;
    info.data = ttbin->gps_records;
    info.max_count = ttbin->gps_record_count;
    info.current_count = 0;

    /* setup the transaction */
    curl_easy_setopt(curl, CURLOPT_URL, "https://mysports.tomtom.com/tyne/dem/fixmodel");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, str - post_data);
    curl_easy_setopt(curl, CURLOPT_POST, 1);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "TomTom");
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 50);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &info);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_data);

    /* perform the transaction */
    result = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK)
    {
        fprintf(stderr, "Unable to download elevation data: %d\n", result);
        return;
    }
}
