/*
 * Phoenix-RTOS
 *
 * HIL scenario CSV format, written by hilgen and sent by hilsim
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _HILCSV_H_
#define _HILCSV_H_


/*
 * Lines starting with '#' are comments, the first other line is HILCSV_HEADER, then one row per frame period.
 * idx - row number, consecutive from 0, hilsim dump refers to scenario rows by it
 * time_us - scenario time of the row [us], rows are equally spaced, the step is the frame period
 * other columns - sensor readings in the imu sensor frame, as sent in hils_statePld_t (see hilsim.c),
 * floats printed with HILCSV_FLT so that they are parsed back to the same value
 */
#define HILCSV_HEADER "idx,time_us,p,q,r,axm,aym,azm,lon,lat,alt,mx,my,mz,pStat,pTot"
#define HILCSV_COLS   16
#define HILCSV_FLT    "%.9g"


#endif
