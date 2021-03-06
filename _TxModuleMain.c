/*
 * File:   _TxModuleMain.c
 * Author: p2773
 *
 * Created on 18. Dezember 2012, 10:01
 */

#include <stdio.h>
#include <stdlib.h>
#include <plib.h>
#include <p32xxxx.h>

#include "adf7023_mint.h"
#include "si5326.h"
#include "configandmux.h"
#include "timestamping.h"
#include "switching.h"
#include "_TxModuleMain.h"


#define tPREAMBEL_LEN       32  //byte
#define tPREAMBEL           0x55
#define tSYNCBYTE0          0x33
#define tSYNCBYTE1          0x33
#define tSYNCBYTE2          0xA6
#define tPAYLOADLEN         32   //240 //byte

#define TX_COUNTER_PERIOD   3932100 //Transmitter: 60 turns with 0xFFFF(+1) timer setting => 60*65536 = 3932160
                                    //empirical: it's rather 60 x 65535 (i.e. 0xFFFF not 0xFFF+1) ==> 3932100
                                    //this was found by applying the SAME clock source (f-generator) to TX and RX.
                                    //If TX_COUNTER_PERIOD = 3932160 then the error for each time measure between two IRQs is around 60.
#define VCO_FREQ            12288000
#define TMEAS_BUFFER_SIZE   512  //==> measuring period = TMEAS_BUFFER_SIZE*TX_COUNTER_PERIOD/VCO_FREQ (e.g. 163.84s);
                                 //caution: largest number on PIC32: 2^32. if buffer is to long the elapsed time between two points is larger than 2^32.
                                 //TMEAS_BUFFER_SIZE GOT TO BE 2^X!
#define OUTLIER_THRESH      (TX_COUNTER_PERIOD >> 10)
#define BUF_SUM_REF         (TX_COUNTER_PERIOD * TMEAS_BUFFER_SIZE)

volatile BOOL rxDetected;
//volatile int TimerVals[(0x01<<MA_SIZE)];
volatile unsigned int counterValue;
volatile unsigned int counterValueOld;
volatile unsigned int counterOverflow;

volatile unsigned int stallRecover;



/*function: fillBuffer*/
BOOL fillBuffer(UINT32 val, UINT32 *bufSum, UINT32 *buf, int *bfIdx)
{
    BOOL full = FALSE;

    *bufSum += val;
    buf[*bfIdx] = val;
    (*bfIdx)++;
    if (*bfIdx == TMEAS_BUFFER_SIZE){
        full = TRUE;
        *bfIdx = 0;
    }
    return full;
}

/*function: updateBuffer*/
BOOL updateBuffer(UINT32 val, UINT32 *bufSum, UINT32 *buf, int *bfIdx)
{
    *bufSum = *bufSum - buf[*bfIdx] + val;
    buf[*bfIdx] = val;
    (*bfIdx)++;
    *bfIdx &= (TMEAS_BUFFER_SIZE-1); //equals: bfIdx = bfIdx % TMEAS_BUFFER_SIZE if TMEAS_BUFFER_SIZE is 2^x

    return 0;
}


/*
 *
 */
int main(void) {

    BOOL bOk = TRUE;
    TyMCR MCRregisters;
    unsigned char dummyDat;
    UINT32 counterDiff;
    float measTurns_float;
    int measTurns_int;
    int tarrInd = 0;
    int tmpInd = 0;
    unsigned int pbclockfreq;
    int tmpC = 0;
    UINT32 filtOut;
    UINT32 expectedValue;
    int skippedFirst = 0;
    UINT32 buf[TMEAS_BUFFER_SIZE];
    UINT32 bufSum=0;
    BOOL bufferFull = FALSE;
    unsigned int bfIdx=0;
    UINT32 cDiff;
    UINT32 res;
    UINT32 thisVal;
    int outlier=0;
    UINT32 bufSumRef = BUF_SUM_REF;
    float fErrorArray[TMEAS_BUFFER_SIZE];
    float tmp_f1;
    int tmp_i1;
    int i;
    UINT8 tsData_8[PKT_MAX_PKT_LEN];
    UINT32 tsData_32;
    UINT32 tmpTsData[10];
    int mn;

    /*debugging*/
    unsigned char MCRByte;
    unsigned char RetVal;
    BOOL retBool;
    ADFSTA_Reg ADStat;

    unsigned int i2c_address;
    int i2c_data;
    int i2c_rcv1;
    int i2c_rcv2;
    unsigned char i2cRcv[2];
    /**/

    counterOverflow = 0;
    counterValue = 0;
    counterValueOld = 0;
    

    pbclockfreq = SYSTEMConfig(GetSystemClock(), SYS_CFG_WAIT_STATES | SYS_CFG_PCACHE);
    //SYSTEMConfigWaitStatesAndPB()
    DDPCONbits.JTAGEN = 0; //disable JTAG



    /*configure phase 1*/
    SwitchOffSport();
    pinMux01();
    mPORTCClearBits(BIT_0); //set CLK_alt low (start off with low clock)
    SPI1_configMaster();
    SwitchADFSpi2Spi1();



    /*TEST NEW PWM*/
    OpenOC1( OC_ON | OC_TIMER2_SRC | OC_PWM_FAULT_PIN_DISABLE | OC_TIMER_MODE16, 0, 0);
    OpenTimer2( T2_ON | T2_PS_1_1 | T2_SOURCE_INT, 0xFFFF);
    SetDCOC1PWM(0x7FFF); //50% duty cycle
    /**************/

    /*TEST CNTL Pins*/
    while(1){
        mPORTBToggleBits(BIT_13);
        mPORTAToggleBits(BIT_1);
        mPORTBToggleBits(BIT_6);
    }
    /****************/

    /********************************************************/
    /***********I2C/SMBus************************************/

    //Enable I2C channel and set the baud rate to BRG_VAL)
    //while(1);

    OpenI2C2( I2C_EN | I2C_SM_EN | I2C_SLW_DIS | I2C_ACK_EN, ((pbclockfreq/2/50000)-2) ); //50000

    i2c_address = 0x16; //charge = 0x12, gauge = 0x16

    IdleI2C2();
    StartI2C2();	        //Send the Start Bit
    IdleI2C2();		//Wait to complete
    MasterWriteI2C2(i2c_address | 0);  //Sends the slave address over the I2C line.  This must happen first so the
                                             //proper slave is selected to receive data.
    IdleI2C2();	        //Wait to complete
    MasterWriteI2C2(0x0d);  //SBS command: reltaveStateOfCharge
    IdleI2C2();		//Wait to complete

    RestartI2C2();									//Restart signal
    //while(I2C2CONbits.RSEN ); 						//Wait till Restart sequence is completed
    //for(i=0;i<1000;i++);
    IdleI2C2();
    MasterWriteI2C2(i2c_address | 1);
    IdleI2C2();

    MastergetsI2C2(2,i2cRcv,1000);
    //i2c_rcv1 = MasterReadI2C2();		//Read in a value
    //IdleI2C2();
    //i2c_rcv2 = MasterReadI2C2();		//Read in a value

    IdleI2C2();
    StopI2C2();	        //Send the Stop condition
    IdleI2C2();	        //Wait to complete
    CloseI2C2();

    /*slave*/
    /*
     http://hades.mech.northwestern.edu/index.php/PIC32MX:_I2C_Communication_between_PIC32s
     */


    while(1);
    /********************************************************/





    /*set up ADF7023*/
    ADF_Init();
    ADF_MCRRegisterReadBack(&MCRregisters); //read back the MCRRegisters

    /*set up PWM*/
    //use OutputCompare1 (OC1) on RB3 (PIN24) -> PPS!
    OpenOC1( OC_ON | OC_TIMER2_SRC | OC_PWM_FAULT_PIN_DISABLE | OC_TIMER_MODE16, 0, 0);
    OpenTimer2( T2_ON | T2_PS_1_1 | T2_SOURCE_INT, 0xFFFF);
    SetDCOC1PWM(0x7FFF); //50% duty cycle

    /*-----------INTERRUPTS----------------------------------------------*/
    /*config interrupt for TX DONE on IRQ3*/
    INTConfigureSystem(INT_SYSTEM_CONFIG_MULT_VECTOR);

    INTSetVectorPriority(INT_VECTOR_EX_INT(1), INT_PRIORITY_LEVEL_3); //set INT controller priority
    INTSetVectorSubPriority(INT_VECTOR_EX_INT(1), INT_SUB_PRIORITY_LEVEL_3); //set INT controller sub-priority
    INTCONbits.INT1EP = 1; //edge polarity -> rising edge
    IFS0 &= ~0x0100; //clear interrupt flag
    IEC0 |= 0x0100; //enable INT1 interrupt

    /*config counter interrupt (TIMER1) (for counting external VCO edges)*/
    //PORTSetPinsDigitalIn(IOPORT_A, BIT_4);   //T1CK
    //OpenTimer1(T1_ON | T1_SOURCE_EXT | T1_PS_1_1, 0xFFFF); //no prescalor other than 1_1 work's?!
    //ConfigIntTimer1(T1_INT_ON | T1_INT_PRIOR_1);
    
    /*config counter interrupt (TIMER1) (for counting internal clock edges)*/
    OpenTimer1(T1_ON | T1_SOURCE_EXT | T1_PS_1_1, 0xFFFF); //no prescalor other than 1_1 work's?!
    ConfigIntTimer1(T1_INT_ON | T1_INT_PRIOR_1);


    /*set up Timer 3 (generate test-clock)*/
    /*
    PORTSetPinsDigitalOut(IOPORT_B, BIT_13);    //PIC_CLK_OUT
    clkCounter = pbclockfreq / OUT_FREQUENCY;
    OpenTimer3(T3_ON | T3_SOURCE_INT | T3_PS_1_1, clkCounter); //no prescalor other than 1_1 work's?!
    ConfigIntTimer3(T3_INT_ON | T3_INT_PRIOR_2);
    */

    INTEnableInterrupts();
    /*------------------------------------------------------------------*/

    /*ADF: Go to RX state*/
    stallRecover = 0;
    rxDetected = FALSE;
    bOk = bOk && ADF_GoToRxState();

    expectedValue = TX_COUNTER_PERIOD;
    filtOut = expectedValue; //to minimize settling time

    mn = 0;
    while(1){
        //bOk = bOk & ADF_MMapRead(MCR_interrupt_source_0_Adr, 0x01, &MCRByte);
        //if (MCRByte & 0x02){
        //mPORTBToggleBits(BIT_2);
        
        if (rxDetected){

            if (skippedFirst){

                /*first test: read received data from packet ram*/
                bOk = bOk && ADF_MMapRead(PKT_RAM_BASE_PTR, PKT_MAX_PKT_LEN, tsData_8);
                tsData_32 = tsData_8[3];
                tsData_32 = (tsData_32 << 8) | tsData_8[2];
                tsData_32 = (tsData_32 << 8) | tsData_8[1];
                tsData_32 = (tsData_32 << 8) | tsData_8[0];
                
                tmpTsData[mn] = tsData_32;
                mn++;
                if (mn == 10){
                    bOk = TRUE;
                    mn = 0;
                }

                
   
                /*----------------------------------------------*/


                /*simple low pass filter*/
                /*
                tmpFilt = (filtOut >> FILT_SZ);
                filtOut -= tmpFilt;
                filtOut += (counterDiff >> FILT_SZ);
                */


                /*compute counter difference between old value and new value*/
                if (counterOverflow){
                    counterDiff = (0xFFFF - counterValueOld) + (counterOverflow-1)*0xFFFF + counterValue;
                }else{
                    counterDiff = counterValue - counterValueOld;
                }

                /*compute turns*/
                /*measTurns_float = counterDiff/(float)TX_COUNTER_PERIOD;
                measTurns_int = (int)(measTurns_float + 0.5f);
                if (measTurns_int){
                    counterDiff = counterDiff/measTurns_int;
                }*/
                
                measTurns_float = counterDiff/(float)TX_COUNTER_PERIOD;
                measTurns_int = (int)(measTurns_float + 0.5f);
                if (measTurns_int){
                    cDiff = counterDiff/measTurns_int; //FLOORED VALUE!
                }else{
                    outlier = 1;
                    measTurns_int = 1; //just so that we don't divide by 0 in the next line
                }    
                
                res = counterDiff - (cDiff*measTurns_int); //residual               
                if (res < OUTLIER_THRESH && outlier == 0){
                    /*Fill buffer with value(s). If packets have been skipped,
                     *distribute values equally across buffer entries (no rounding)*/
                    for (i = measTurns_int; i > 0; i--){
                        thisVal = counterDiff/i;
                        if (bufferFull){
                            updateBuffer(thisVal, &bufSum, buf, &bfIdx);
                            /*estimate frequency error [ppm]*/
                            tmp_i1 = bufSum - bufSumRef; //signed int!
                            tmp_f1 = (float)1000000 * (float)tmp_i1;
                            fErrorArray[bfIdx-1] = tmp_f1 / (float)bufSumRef;
                            if (bfIdx == 50){
                                bfIdx = 400;
                            }
                        }else{
                            bufferFull = fillBuffer(thisVal, &bufSum, buf, &bfIdx);
                        }

                        counterDiff -= thisVal;
                    }

                    

                    /*filter?*/
                    
                    /*write to PWM register*/

                }else{
                    //outlier!
                    outlier = 0;
                    //keep the same estimated frequency
                }


                //tempYOutputArray[tmpInd] = filtOut - expectedValue;
                //arr1[tmpInd] = counterDiff;
                //arr2[tmpInd] = filtOut;
                //tmpInd++;
                //if (tmpInd == 2000){
                //    tmpInd = 0;
                //    tmpC++;
                //    if (tmpC == 3){
                //        tmpC = 0;
                //    }
                //}
                
            }
            skippedFirst = 1;

            


            /*moving average filter
            if (MAWindowFull){
                maSum = maSum - TimerVals[tarrInd] + counterDiff;
                TimerVals[tarrInd] = counterDiff;
                tarrInd++;
                tarrInd &= ((0x01<<MA_SIZE)-1); //equals: tarrInd = tarrInd % 2^MA_SIZE
                maOutput = maSum >> MA_SIZE; //devide by 2^MA_SIZE
            }else{
                maSum = maSum + counterDiff; //fill MA window
                TimerVals[tarrInd] = counterDiff;
                maOutput = maSum >> MA_SIZE; //devide by 2^MA_SIZE
                tarrInd++;
                if (tarrInd == (0x01<<MA_SIZE)){
                    MAWindowFull = TRUE;
                    tarrInd = 0;
                }
            }*/

            /*write to PWM register*/
            //TX_COUNTER_PERIOD / maOutput;
            //SetDCOC1PWM(0x7FFF);

            //Temp
            /*
            tempMAOutputArray[tmpInd] = maOutput;
            tmpInd++;
            if (tmpInd == 80){
                tmpInd = 0;
            }*/


            /*reset counters*/
            counterOverflow = 0;
            counterValueOld = counterValue;
            //rxDetected = FALSE;

            /*Clear ADF8023 Interrupt*/
            dummyDat = 0xFF;
            bOk = bOk & ADF_MMapWrite(MCR_interrupt_source_0_Adr, 0x1, &dummyDat); //clear all interrupts in source 0 by writing 1's

            //debugging
            //RetVal = ADF_MMapRead(MCR_interrupt_source_0_Adr, 0x01, &MCRByte);
            //retBool = ADF_ReadStatus(&ADStat);

            //if (stallRecover==1){
            //    stallRecover = 0;
            //}

            /*Bring ADF back to RxState*/
            rxDetected = FALSE;
            bOk = bOk && ADF_GoToRxState();
            
            
        }
    }

    return (EXIT_SUCCESS);
}


void __ISR(_EXTERNAL_1_VECTOR, ipl3) INT1Interrupt()
{
   //read and reset counter value TMR1
   while(T1CON & 0x0800); //check T1CON.TWIP and wait until theres no write to TMR1 in progess
   counterValue = TMR1;
   rxDetected = TRUE;
   //mPORTBToggleBits(BIT_2);
   mINT1ClearIntFlag();

}

void __ISR(_TIMER_1_VECTOR, ipl1) T1Interrupt()
{
   //unsigned char MCRByte;
   //unsigned char RetVal;
   //BOOL retBool;
   //ADFSTA_Reg ADStat;

   counterOverflow++;
   //stallRecover = 0;
   //if (counterOverflow > 5000){
       //RetVal = ADF_MMapRead(MCR_interrupt_source_0_Adr, 0x01, &MCRByte);
       //retBool = ADF_ReadStatus(&ADStat);
       //counterOverflow = 0;
   //    rxDetected = TRUE;
   //    stallRecover = 1;
       //mPORTBToggleBits(BIT_2);
   //}
   mPORTBToggleBits(BIT_2);
   mT1ClearIntFlag();
}


void __ISR(_TIMER_3_VECTOR, ipl2) T3Interrupt()
{
   //mPORTBToggleBits(BIT_13);
   mT3ClearIntFlag();
}
