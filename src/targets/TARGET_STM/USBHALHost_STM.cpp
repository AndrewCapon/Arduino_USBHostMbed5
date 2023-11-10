/* mbed USBHost Library
 * Copyright (c) 2006-2013 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef TARGET_STM
#pragma GCC optimize ("O0")

#if defined(TARGET_PORTENTA_H7)
#define USBx_BASE   USB2_OTG_FS_PERIPH_BASE
#elif defined(TARGET_GIGA)
#define USBx_BASE   USB1_OTG_HS_PERIPH_BASE
#else
#define USBx_BASE   USB1_OTG_HS_PERIPH_BASE
#endif

#include "mbed.h"
#include "USBHost/USBHALHost.h"
#include "USBHost/dbg.h"
#include "pinmap.h"
#include "mbed_chrono.h"

#include "USBHALHost_STM.h"

void HAL_HCD_Connect_Callback(HCD_HandleTypeDef *hhcd)
{
    USBHALHost_Private_t *priv = (USBHALHost_Private_t *)(hhcd->pData);
    USBHALHost *obj = priv->inst;
    void (USBHALHost::*func)(int hub, int port, bool lowSpeed, USBHostHub * hub_parent) = priv->deviceConnected;
    (obj->*func)(0, 1, 0, NULL);
}
void HAL_HCD_Disconnect_Callback(HCD_HandleTypeDef *hhcd)
{
    USBHALHost_Private_t *priv = (USBHALHost_Private_t *)(hhcd->pData);
    USBHALHost *obj = priv->inst;
    void (USBHALHost::*func1)(int hub, int port, USBHostHub * hub_parent, volatile uint32_t addr) = priv->deviceDisconnected;
    (obj->*func1)(0, 1, (USBHostHub *)NULL, 0);
}
int HAL_HCD_HC_GetDirection(HCD_HandleTypeDef *hhcd, uint8_t chnum)
{
    /*  useful for transmission */
    return hhcd->hc[chnum].ep_is_in;
}

uint32_t HAL_HCD_HC_GetMaxPacket(HCD_HandleTypeDef *hhcd, uint8_t chnum)
{
    /*  useful for transmission */
    return hhcd->hc[chnum].max_packet;
}

void  HAL_HCD_EnableInt(HCD_HandleTypeDef *hhcd, uint8_t chnum)
{
  if(chnum == 1)
    digitalWrite(PC_0, HIGH);

    USB_OTG_GlobalTypeDef *USBx = hhcd->Instance;
    USBx_HOST->HAINTMSK |= (1 << chnum);
}


void  HAL_HCD_DisableInt(HCD_HandleTypeDef *hhcd, uint8_t chnum)
{
  if(chnum == 1)
    digitalWrite(PC_0, LOW);

    USB_OTG_GlobalTypeDef *USBx = hhcd->Instance;
    USBx_HOST->HAINTMSK &= ~(1 << chnum);
}

uint32_t HAL_HCD_HC_GetType(HCD_HandleTypeDef *hhcd, uint8_t chnum)
{
    /*  useful for transmission */
    return hhcd->hc[chnum].ep_type;
}

// The urb_state parameter can be one of the following according to the ST documentation, but the explanations
// that follow are the result of an analysis of the ST HAL / LL source code, the Reference Manual for the
// STM32H747 microcontroller, and the source code of this library:
//
//  - URB_ERROR    = various serious errors that may be hard or impossible to recover from without pulling
//                   the thumb drive out and putting it back in again, but we will try
//  - URB_IDLE     = set by a call to HAL_HCD_HC_SubmitRequest(), but never used by this library
//  - URB_NYET     = never actually used by the ST HAL / LL at the time of writing, nor by this library
//  - URB_STALL    = a stall response from the device - but it is never handled by the library and will end up
//                   as a timeout at a higher layer, because of an ep_queue.get() timeout, which will activate
//                   error recovery indirectly
//  - URB_DONE     = the transfer completed normally without errors
//  - URB_NOTREADY = a NAK, NYET, or not more than a couple of repeats of some of the errors that will
//                   become URB_ERROR if they repeat several times in a row
//

//#if ARC_USB_FULL_SIZE
extern "C" void LogicUint7(uint8_t u);
//#endif

 
#if ARC_TICKER_BASED
void USBHALHost::tickerCallback(void)
{
  HCD_HandleTypeDef    *pHcd  = (HCD_HandleTypeDef *)usb_hcca;
  if(pHcd->State != HAL_HCD_STATE_BUSY)
  {
    USBHALHost_Private_t *pPriv = (USBHALHost_Private_t *)(pHcd->pData);

    // 10 host channels
    digitalWrite(PC_3, HIGH);
    for(uint8_t uChannel=0; uChannel <11; uChannel++)
    {
      USB_OTG_URBStateTypeDef urbState = pHcd->hc[uChannel].urb_state;
      LogicUint7(urbState);

      HCTD    *pTransferDescriptor  = (pPriv->addr[uChannel] == 0xffffffff) ? nullptr : (HCTD *)pPriv->addr[uChannel];
      uint32_t uEndpointType        = pHcd->hc[uChannel].ep_type;
      uint32_t uEndpointDirection   = pHcd->hc[uChannel].ep_is_in;;

      if(pTransferDescriptor)
      {
        if (uEndpointType == EP_TYPE_INTR)
        {
          // Disable the channel interupt
          pTransferDescriptor->state = USB_TYPE_IDLE;
          HAL_HCD_DisableInt(pHcd, uChannel);
        } 
        else 
        {
          // Handle USB NAKs
          if(urbState == URB_NOTREADY)
          {
            // retry Acks imediately
            // retry Bulk and Control after uRetryCounts ms
            if ((uEndpointType == EP_TYPE_BULK) || (uEndpointType == EP_TYPE_CTRL)) 
            {
              constexpr uint32_t uRetryCount = 5;

              pTransferDescriptor->retry++;

              if(urbState == URB_NOTREADY)
              {
                volatile uint32_t transferred = HAL_HCD_HC_GetXferCount(pHcd, uChannel);

                LogicUint7(0x40 + transferred);

                if((pTransferDescriptor->retry == uRetryCount) || (pTransferDescriptor->size==0))
                {
                  // Submit the same request again, because the device wasn't ready to accept the last one
                  // we need to be aware of any data that has already been transferred as it wont be again by the look of it.
                  pTransferDescriptor->currBufPtr += transferred;
                  pTransferDescriptor->size -= transferred;
                  pTransferDescriptor->retry = 0;
                  uint32_t uLength = pTransferDescriptor->size;

                  HAL_HCD_HC_SubmitRequest(pHcd, uChannel, uEndpointDirection, uEndpointType, !pTransferDescriptor->setup, (uint8_t *) pTransferDescriptor->currBufPtr, uLength, 0);
                  HAL_HCD_EnableInt(pHcd, uChannel);
                }
              }
            }
          }
          else
          {
            // set the transfer descriptor state based on the URB state
            switch(urbState)
            {
              case URB_IDLE:
              case URB_DONE:  pTransferDescriptor->state = USB_TYPE_IDLE; break;
              case URB_ERROR: pTransferDescriptor->state = USB_TYPE_ERROR; break;
              default:        pTransferDescriptor->state = USB_TYPE_PROCESSING; break; 
            }
          }
        }

        if (pTransferDescriptor->state == USB_TYPE_IDLE) 
        {
          // reset retry count
          pTransferDescriptor->retry = 0;

          // Update transfer descriptor buffer pointer
          pTransferDescriptor->currBufPtr += HAL_HCD_HC_GetXferCount(pHcd, uChannel);

          // Call transferCompleted on correct object
          void (USBHALHost::*func)(volatile uint32_t addr) = pPriv->transferCompleted;
          (pPriv->inst->*func)(reinterpret_cast<std::uintptr_t>(pTransferDescriptor));
        }
      }
    }
  }
  digitalWrite(PC_3, LOW);
}
#else
void HAL_HCD_HC_NotifyURBChange_Callback(HCD_HandleTypeDef *hhcd, uint8_t chnum, HCD_URBStateTypeDef urb_state)
{
    USBHALHost_Private_t *priv = (USBHALHost_Private_t *)(hhcd->pData);
    USBHALHost *obj = priv->inst;
    void (USBHALHost::*func)(volatile uint32_t addr) = priv->transferCompleted;

    uint32_t addr = priv->addr[chnum];
    uint32_t max_size = HAL_HCD_HC_GetMaxPacket(hhcd, chnum);
    uint32_t type = HAL_HCD_HC_GetType(hhcd, chnum);
    uint32_t dir = HAL_HCD_HC_GetDirection(hhcd, chnum);
    

    uint32_t length;
    if ((addr != 0)) {
        HCTD *td = (HCTD *)addr;
		    LogicUint7(0x70 + urb_state);
		    //LogicUint7(0x50 + chnum);
        digitalWrite(PA_7, HIGH);

#if ARC_USB_FULL_SIZE
        constexpr uint32_t uRetryCount = 10000/20; // 10 ms (TODO: should be done with timer, investigate)
        if ((type == EP_TYPE_BULK) || (type == EP_TYPE_CTRL)) 
        {
          td->retry++;

          if(urb_state == URB_NOTREADY)
          {
            volatile uint32_t transferred = HAL_HCD_HC_GetXferCount(hhcd, chnum);

            LogicUint7(0x40 + transferred);

            if((td->retry > uRetryCount) || (td->size==0))
            {
              // Submit the same request again, because the device wasn't ready to accept the last one
              // we need to be aware of any data that has already been transferred as it wont be again by the look of it.
              // Also only do this once until if (td->state == USB_TYPE_IDLE) resets it below
              td->currBufPtr += transferred;
              td->size -= transferred;
              td->retry = 0;
              length = td->size;

              HAL_HCD_HC_SubmitRequest(hhcd, chnum, dir, type, !td->setup, (uint8_t *) td->currBufPtr, length, 0);
              HAL_HCD_EnableInt(hhcd, chnum);
            }
          }
        }
#else
        if ((type == EP_TYPE_BULK) || (type == EP_TYPE_CTRL)) {
            switch (urb_state) {
                case URB_DONE:
#if defined(MAX_NOTREADY_RETRY)
                    td->retry = 0;
#endif
                    if (td->size >  max_size) {
                        /*  enqueue  another request */
                        td->currBufPtr += max_size;
                        td->size -= max_size;
                        td->retry=0;
                        length = td->size <= max_size ? td->size : max_size;
                 		    LogicUint7(0x50 + length);
                        HAL_HCD_HC_SubmitRequest(hhcd, chnum, dir, type, !td->setup, (uint8_t *) td->currBufPtr, length, 0);
                        HAL_HCD_EnableInt(hhcd, chnum);
                        LogicUint7(0);
                        digitalWrite(PA_7, LOW);
                        return;
                    }
                    break;
                case  URB_NOTREADY:
#if defined(MAX_NOTREADY_RETRY)
                    if (td->retry < MAX_NOTREADY_RETRY) {
                        td->retry++;
#endif
                        if(((td->retry % 500) == 0) || (td->size==0) || (td->retry == 1))
                        {
                          // Submit the same request again, because the device wasn't ready to accept the last one
                          length = td->size <= max_size ? td->size : max_size;
                          HAL_HCD_HC_SubmitRequest(hhcd, chnum, dir, type, !td->setup, (uint8_t *) td->currBufPtr, length, 0);
                          HAL_HCD_EnableInt(hhcd, chnum);
                        }
                        LogicUint7(0);
                        digitalWrite(PA_7, LOW);
                        return;
#if defined(MAX_NOTREADY_RETRY)
                    } else {
                        // MAX_NOTREADY_RETRY reached, so stop trying to resend and instead wait for a timeout at a higher layer
                    }
#endif
                    break;
            }
        }
#endif
        if ((type == EP_TYPE_INTR)) {
            /*  reply a packet of length NULL, this will be analyze in call back
             *  for mouse or hub */
            td->state = USB_TYPE_IDLE ;
            HAL_HCD_DisableInt(hhcd, chnum);

        } else {
            if (urb_state == URB_DONE) {
                td->state = USB_TYPE_IDLE;
            }
            else if (urb_state == URB_ERROR) {
                // While USB_TYPE_ERROR in the endpoint state is used to activate error recovery, this value is actually never used.
                // Going here will lead to a timeout at a higher layer, because of ep_queue.get() timeout, which will activate error
                // recovery indirectly.
                td->state = USB_TYPE_ERROR;
            } else {
                td->state = USB_TYPE_PROCESSING;
            }
        }
        if (td->state == USB_TYPE_IDLE) {
#if ARC_USB_FULL_SIZE
            td->retry = 0;
#endif
            td->currBufPtr += HAL_HCD_HC_GetXferCount(hhcd, chnum);
            (obj->*func)(addr);
        }
    } else {
        if (urb_state != 0) {
            //USB_DBG_EVENT("spurious %d %d", chnum, urb_state);
        }
    }
//#if ARC_USB_FULL_SIZE	
    LogicUint7(0);
    digitalWrite(PA_7, LOW);
//#endif

}
#endif

USBHALHost *USBHALHost::instHost;

#if defined(TARGET_OPTA)
#include <usb_phy_api.h>
#include <Wire.h>
#endif

void USBHALHost::init()
{
#if defined(TARGET_OPTA)
    get_usb_phy()->deinit();

    Wire1.begin();

    // reset FUSB registers
    Wire1.beginTransmission(0x21);
    Wire1.write(0x5);
    Wire1.write(0x1);
    Wire1.endTransmission();

    Wire1.beginTransmission(0x21);
    Wire1.write(0x5);
    Wire1.write(0x0);
    Wire1.endTransmission();

    delay(100);

    // setup port as SRC
    Wire1.beginTransmission(0x21);
    Wire1.write(0x2);
    Wire1.write(0x3);
    Wire1.endTransmission();

    // enable interrupts
    Wire1.beginTransmission(0x21);
    Wire1.write(0x3);
    Wire1.write(1 << 1);
    Wire1.endTransmission();
#endif

    NVIC_DisableIRQ(USBHAL_IRQn);
    NVIC_SetVector(USBHAL_IRQn, (uint32_t)(_usbisr));
    HAL_HCD_Init((HCD_HandleTypeDef *) usb_hcca);
    control_disable = 0;
    HAL_HCD_Start((HCD_HandleTypeDef *) usb_hcca);
    NVIC_EnableIRQ(USBHAL_IRQn);
    usb_vbus(1);

#endif


}

uint32_t USBHALHost::controlHeadED()
{
    return 0xffffffff;
}

uint32_t USBHALHost::bulkHeadED()
{
    return 0xffffffff;
}

uint32_t USBHALHost::interruptHeadED()
{
    return 0xffffffff;
}

void USBHALHost::updateBulkHeadED(uint32_t addr)
{
}


void USBHALHost::updateControlHeadED(uint32_t addr)
{
}

void USBHALHost::updateInterruptHeadED(uint32_t addr)
{
}


void USBHALHost::enableList(ENDPOINT_TYPE type)
{
    /*  react when the 3 lists are requested to be disabled */
    if (type == CONTROL_ENDPOINT) {
        control_disable--;
        if (control_disable == 0) {
            NVIC_EnableIRQ(USBHAL_IRQn);
        } else {
            //printf("reent\n");
        }
    }
}


bool USBHALHost::disableList(ENDPOINT_TYPE type)
{
    if (type == CONTROL_ENDPOINT) {
        NVIC_DisableIRQ(USBHAL_IRQn);
        control_disable++;
        if (control_disable > 1) {
           //printf("disable reentrance !!!\n");
        }
        return true;
    }
    return false;
}


void USBHALHost::memInit()
{
    usb_hcca = (volatile HCD_HandleTypeDef *)usb_buf;
    usb_edBuf = usb_buf + HCCA_SIZE;
    usb_tdBuf = usb_buf + HCCA_SIZE + (MAX_ENDPOINT * ED_SIZE);
    /*  init channel  */
    memset((void *)usb_buf, 0, TOTAL_SIZE);
    for (int i = 0; i < MAX_ENDPOINT; i++) {
        HCED    *hced = (HCED *)(usb_edBuf + i * ED_SIZE);
        hced->ch_num = i;
        hced->hhcd = (HCCA *) usb_hcca;
    }
}

volatile uint8_t *USBHALHost::getED()
{
    for (int i = 0; i < MAX_ENDPOINT; i++) {
        if (!edBufAlloc[i]) {
            edBufAlloc[i] = true;
            return (volatile uint8_t *)(usb_edBuf + i * ED_SIZE);
        }
    }
    perror("Could not allocate ED\r\n");
    return NULL; //Could not alloc ED
}

volatile uint8_t *USBHALHost::getTD()
{
    int i;
    for (i = 0; i < MAX_TD; i++) {
        if (!tdBufAlloc[i]) {
            tdBufAlloc[i] = true;
            return (volatile uint8_t *)(usb_tdBuf + i * TD_SIZE);
        }
    }
    perror("Could not allocate TD\r\n");
    return NULL; //Could not alloc TD
}


void USBHALHost::freeED(volatile uint8_t *ed)
{
    int i;
    i = (ed - usb_edBuf) / ED_SIZE;
    edBufAlloc[i] = false;
}

void USBHALHost::freeTD(volatile uint8_t *td)
{
    int i;
    i = (td - usb_tdBuf) / TD_SIZE;
    tdBufAlloc[i] = false;
}

using namespace mbed::chrono_literals;

void USBHALHost::resetRootHub()
{
    // Initiate port reset
    rtos::ThisThread::sleep_for(200);
    HAL_HCD_ResetPort((HCD_HandleTypeDef *)usb_hcca);
}


void USBHALHost::_usbisr(void)
{
    if (instHost) {
        instHost->UsbIrqhandler();
    }
}

void USBHALHost::UsbIrqhandler()
{
    HAL_HCD_IRQHandler((HCD_HandleTypeDef *)usb_hcca);
}

