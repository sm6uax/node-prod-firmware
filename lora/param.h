#ifndef __PARAM_H__
#define __PARAM_H__

#define PARAM_DEV_EUI	0
#define PARAM_APP_EUI	1
#define PARAM_DEV_KEY	2
#define PARAM_SUOTA	3

#define PARAM_MAX_LEN	16	/* sizeof(devkey) */

void	param_init(void);
int	param_get(int idx, uint8_t *data, uint8_t len);
void	param_set(int idx, uint8_t *data, uint8_t len);

#endif /* __PARAM_H__ */
