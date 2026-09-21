#ifndef DADA_SPI_H
#define DADA_SPI_H

#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/spi.h>
#include <Arduino.h>
#include <string.h>

class DaDa_SPI {
    public:
        DaDa_SPI(spi_inst_t* spi_port, uint cs, uint mosi, uint miso, uint clk, uint handshake_pin, uint speed) : _spi_port{spi_port}, _spi_cs{cs}, _spi_mosi{mosi}, _spi_miso{miso}, _spi_sclk{clk}, _handshake_pin{handshake_pin}, _spi_speed{speed} {
            // claim two dma channels for tx / rx
            dma_tx_spi = dma_claim_unused_channel(true);
            dma_rx_spi = dma_claim_unused_channel(true);

            // configure dma
            ctx_spi = dma_channel_get_default_config(dma_tx_spi);
            channel_config_set_transfer_data_size(&ctx_spi, DMA_SIZE_8);
            channel_config_set_dreq(&ctx_spi, spi_get_dreq(_spi_port, true));
            channel_config_set_read_increment(&ctx_spi, true);
            channel_config_set_write_increment(&ctx_spi, false);

            crx_spi = dma_channel_get_default_config(dma_rx_spi);
            channel_config_set_transfer_data_size(&crx_spi, DMA_SIZE_8);
            channel_config_set_dreq(&crx_spi, spi_get_dreq(_spi_port, false));
            channel_config_set_read_increment(&crx_spi, false);
            channel_config_set_write_increment(&crx_spi, true);

            // configure handshake pin as input, "ready for transaction from p4", 0 = busy, 1 = ready
            gpio_init(_handshake_pin);
            gpio_set_dir(_handshake_pin, GPIO_IN);
            gpio_pull_down(_handshake_pin);

            // Initialize SPI bus and slave interface
            spi_init(_spi_port, _spi_speed);
            gpio_set_function(_spi_miso, GPIO_FUNC_SPI);
            gpio_set_function(_spi_sclk, GPIO_FUNC_SPI);
            gpio_set_function(_spi_mosi, GPIO_FUNC_SPI);
            gpio_set_function(_spi_cs, GPIO_FUNC_SPI);
            // this mode allows to assert CS for entire transfer by spi hw peripheral
            // only mode supported in this lib
            spi_set_format(_spi_port, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);
        }
        ~DaDa_SPI() {
            spi_deinit(_spi_port);
            dma_channel_unclaim(dma_tx_spi);
            dma_channel_unclaim(dma_rx_spi);
        }
        bool IsBusy(){
            return dma_channel_is_busy(dma_tx_spi) || dma_channel_is_busy(dma_rx_spi);
        }
        void WaitUntilDMADoneBlocking(){
            while(IsBusy()) tight_loop_contents();
        }
        // A peer that resets holds the handshake low for its whole boot; without a
        // liveness predicate the wait is unbounded, with one it ends (false) as soon
        // as the predicate reports the peer dead while the handshake is low.
        typedef bool (*PeerAlivePredicate)(void* ctx);
        void SetPeerAlivePredicate(PeerAlivePredicate alive, void* ctx){
            _peer_alive = alive;
            _peer_alive_ctx = ctx;
        }
        __noinline bool WaitUntilP4IsReady(){
            bool alive = PeerAlive();
            while(!gpio_get(_handshake_pin)){
                if(!alive) return false;
                tight_loop_contents();
                alive = PeerAlive();
            }
            return true;
        }
        bool GetP4Ready(){
            return gpio_get(_handshake_pin);
        }
        // false = the peer died under the wait: nothing was clocked and rx_buf is zeroed.
        // The blocking calls last a whole frame; inlining them only multiplies their code.
        __noinline bool TransferBlocking(uint8_t* tx_buf, uint8_t* rx_buf, uint len){
            WaitUntilDMADoneBlocking(); // wait until previous transfer is done
            if(!WaitUntilP4IsReady()){
                memset(rx_buf, 0, len);
                return false;
            }
            StartDMA(tx_buf, rx_buf, len); // start DMA transfer
            WaitUntilDMADoneBlocking(); // wait until transfer is done
            return true;
        }
        __noinline bool TransferBlockingDelayed(uint8_t* tx_buf, uint8_t* rx_buf, uint len, uint delay_us=15){
            WaitUntilDMADoneBlocking(); // wait until previous transfer is done
            // delay here works
            // if (delay_us > 0) busy_wait_us_32(delay_us);
            if(!WaitUntilP4IsReady()){
                memset(rx_buf, 0, len);
                return false;
            }
            // delay here fails
            StartDMA(tx_buf, rx_buf, len); // start DMA transfer
            WaitUntilDMADoneBlocking(); // wait until transfer is done
            // delay here works
            if (delay_us > 0) busy_wait_us_32(delay_us);
            return true;
        }
        void StartDMA(uint8_t* tx_buf, uint8_t* rx_buf, uint len){
            // configure DMA
            dma_channel_configure(dma_tx_spi, &ctx_spi,
                                  &spi_get_hw(_spi_port)->dr, // write address
                                  tx_buf, // read address
                                  len, // element count (each element is of size transfer_data_size)
                                  false); // don't start yet
            dma_channel_configure(dma_rx_spi, &crx_spi,
                                  rx_buf, // write address
                                  &spi_get_hw(_spi_port)->dr, // read address
                                  len, // element count (each element is of size transfer_data_size)
                                  false); // don't start yet


            // start DMA
            dma_start_channel_mask((1u << dma_tx_spi) | (1u << dma_rx_spi));
        }
private:
    bool PeerAlive(){
        return _peer_alive == nullptr || _peer_alive(_peer_alive_ctx);
    }
    spi_inst_t * _spi_port {nullptr};
    PeerAlivePredicate _peer_alive {nullptr};
    void* _peer_alive_ctx {nullptr};
    uint _spi_cs, _spi_mosi, _spi_miso, _spi_sclk, _spi_speed, _handshake_pin;
    uint dma_tx_spi;
    uint dma_rx_spi;
    dma_channel_config ctx_spi;
    dma_channel_config crx_spi;
};

#endif //DADA_SPI_H