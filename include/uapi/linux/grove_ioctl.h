#ifndef __GROVE_IOCTL_H__
#define __GROVE_IOCTL_H__

#define MAGIC 'M'

#define SET_COLOR           0x01
#define GET_COLOR           0x02
#define CLEAR_LCD           0x03
#define WRITE_FIRST_LINE    0x04
#define WRITE_SECOND_LINE   0x05
#define READ_LCD            0x06
#define GET_LINE_SIZE       0x07


#define GROVE_SET_COLOR         _IOW (MAGIC, SET_COLOR, struct color_t) 
#define GROVE_GET_COLOR         _IOR (MAGIC, GET_COLOR, struct color_t)
#define GROVE_CLEAR_LCD         _IO  (MAGIC, CLEAR_LCD)
#define GROVE_WRITE_FIRST_LINE  _IOW (MAGIC, WRITE_FIRST_LINE, struct string_t)
#define GROVE_WRITE_SECOND_LINE _IOW (MAGIC, WRITE_SECOND_LINE, struct string_t)
#define GROVE_READ_LCD          _IOR (MAGIC, READ_LCD, unsigned long)
#define GROVE_GET_LINE_SIZE     _IOR (MAGIC, GET_LINE_SIZE, uint8_t)

#endif /* __GROVE_IOCTL_H__ */
