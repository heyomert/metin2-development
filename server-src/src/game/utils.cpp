#include "stdafx.h"
#include "utils.h"

#include <ma_hash.h>

// MariaDB Connector/C hash API (vendor/mariadb-connector-c-3.4.5/include/ma_crypt.h). Declared here with an
// opaque context because ma_crypt.h needs the connector's crypto-backend macro; the signatures are the same
// for every backend (libmariadb/secure/openssl_crypt.c, win_crypt.c, gnutls_crypt.c).
extern "C"
{
	void* ma_hash_new(unsigned int algorithm);
	void ma_hash_free(void* ctx);
	void ma_hash_input(void* ctx, const unsigned char* buffer, size_t len);
	void ma_hash_result(void* ctx, unsigned char* digest);
}

static int global_time_gap = 0;

time_t get_global_time()
{
	return time(0) + global_time_gap;
}

void set_global_time(time_t t)
{
	global_time_gap = t - time(0);

	char time_str_buf[32];
	snprintf(time_str_buf, sizeof(time_str_buf), "%s", time_str(get_global_time()));

	sys_log(0, "GLOBAL_TIME: %s time_gap %d", time_str_buf, global_time_gap);
}

int dice(int number, int size)
{
	int sum = 0, val;

	if (size <= 0 || number <= 0)
		return (0);

	while (number)
	{
		val = number(1, size);
		sum += val;
		--number;
	}

	return (sum);
}

size_t str_lower(const char * src, char * dest, size_t dest_size)
{
	size_t len = 0;

	if (!dest || dest_size == 0)
		return len;

	if (!src)
	{
		*dest = '\0';
		return len;
	}

	// \0 자리 확보
	--dest_size;

	while (*src && len < dest_size)
	{
		*dest = LOWER(*src); // LOWER 매크로에서 ++나 --하면 안됨!!

		++src;
		++dest;
		++len;
	}

	*dest = '\0';
	return len;
}

void skip_spaces(const char **string)
{   
	for (; **string != '\0' && isnhspace(**string); ++(*string));
}

const char *one_argument(const char *argument, char *first_arg, size_t first_size)
{
	char mark = FALSE;
	size_t first_len = 0;

	if (!argument || 0 == first_size)
	{
		sys_err("one_argument received a NULL pointer!");               
		*first_arg = '\0';
		return NULL;    
	} 

	// \0 자리 확보
	--first_size;

	skip_spaces(&argument);

	while (*argument && first_len < first_size)
	{ 
		if (*argument == '\"')
		{
			mark = !mark;
			++argument; 
			continue;   
		}

		if (!mark && isnhspace(*argument))      
			break;

		*(first_arg++) = *argument;
		++argument;     
		++first_len;
	} 

	*first_arg = '\0';

	skip_spaces(&argument);
	return (argument);
}

const char *two_arguments(const char *argument, char *first_arg, size_t first_size, char *second_arg, size_t second_size)
{
	return (one_argument(one_argument(argument, first_arg, first_size), second_arg, second_size));
}

const char *first_cmd(const char *argument, char *first_arg, size_t first_arg_size, size_t *first_arg_len_result)
{           
	size_t cur_len = 0;
	skip_spaces(&argument);

	// \0 자리 확보
	first_arg_size -= 1;

	while (*argument && !isnhspace(*argument) && cur_len < first_arg_size)
	{
		*(first_arg++) = LOWER(*argument);
		++argument;
		++cur_len;
	}

	*first_arg_len_result = cur_len;
	*first_arg = '\0';
	return (argument);
}

int CalculateDuration(int iSpd, int iDur)
{
	int i = 100 - iSpd;

	if (i > 0) 
		i = 100 + i;
	else if (i < 0) 
		i = 10000 / (100 - i);
	else
		i = 100;

	return iDur * i / 100;
}

int parse_time_str(const char* str)
{
	int tmp = 0;
	int secs = 0;

	while (*str != 0)
	{
		switch (*str)
		{
			case 'm':
			case 'M':
				secs += tmp * 60;
				tmp = 0;
				break;

			case 'h':
			case 'H':
				secs += tmp * 3600;
				tmp = 0;
				break;

			case 'd':
			case 'D':
				secs += tmp * 86400;
				tmp = 0;
				break;

			case '0':
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9':
				tmp *= 10;
				tmp += (*str) - '0';
				break;

			case 's':
			case 'S':
				secs += tmp;
				tmp = 0;
				break;
			default:
				return -1;
		}
		++str;
	}

	return secs + tmp;
}

bool WildCaseCmp(const char *w, const char *s)
{
	for (;;)
	{
		switch(*w)
		{
			case '*':
				if (!w[1])
					return true;
				{
					for (size_t i = 0; i <= strlen(s); ++i)
					{
						if (true == WildCaseCmp(w + 1, s + i))
							return true;
					}
				}
				return false;

			case '?':
				if (!*s)
					return false;

				++w;
				++s;
				break;

			default:
				if (*w != *s)
				{
					if (tolower(*w) != tolower(*s))
						return false;
				}

				if (!*w)
					return true;

				++w;
				++s;
				break;
		}
	}

	return false;
}

static bool sha1_digest(const unsigned char* in, size_t len, unsigned char out[MA_SHA1_HASH_SIZE])
{
	void* ctx = ma_hash_new(MA_HASH_SHA1);
	if (!ctx)
		return false;

	ma_hash_input(ctx, in, len);
	ma_hash_result(ctx, out);
	ma_hash_free(ctx);
	return true;
}

bool mysql_native_password_hash(const char* pw, size_t len, std::string& out)
{
	unsigned char stage1[MA_SHA1_HASH_SIZE];
	unsigned char stage2[MA_SHA1_HASH_SIZE];

	if (!sha1_digest(reinterpret_cast<const unsigned char*>(pw), len, stage1) || !sha1_digest(stage1, sizeof(stage1), stage2))
		return false;

	static const char hex[] = "0123456789ABCDEF";

	out.assign(1, '*');
	for (unsigned char b : stage2)
	{
		out += hex[b >> 4];
		out += hex[b & 0x0F];
	}
	return true;
}
