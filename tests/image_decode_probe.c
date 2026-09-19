// SPDX-License-Identifier: GPL-2.0-only
/* Small target-side probe; fixtures and expected values are supplied externally. */
#include "image_decode.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    struct image_result *result=calloc(1,sizeof(*result));
    if(!result)return 2;
    image_decode(argv[1],0,result);
    int failed=!result->ok;
    if(failed)printf("Error: %s\n",result->error);
    else printf("%u x %u RGB %u %u %u\n",result->width,result->height,result->rgb[0],result->rgb[1],result->rgb[2]);
    free(result); return failed;
}
