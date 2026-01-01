// Intel HD Audio compatible sound card driver for uniOS.
// Developed by Komok050505. 2026.

// Credits:
// - OSDev Wiki - Theory about HDA CODECs and nodes.
// - BleskOS HDA driver - Implementation example and code parts.
// - unionyxx - uniOS.

#include "hda.h"

#include "wav.h"
#include "mp3.h"

#include "debug.h"
#include "timer.h"
#include "unifs.h"

#include "io.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"

#include "kstring.h"
using kstring::memset;
using kstring::memcpy;

static HdAudioDevice hda_info;

bool hda_is_initialized() {
    return hda_info.is_initialized;
}

bool hda_is_paused() {
    return hda_info.is_paused;
}

bool hda_is_playing() {
    return hda_info.is_playing;
}

// Initialize HD Audio sound card. Really huge function!
void hda_init() {
    // Check if it was already initialized.
    if (hda_info.is_initialized) {
        DEBUG_WARN("hda_init called, but it is already initialized!");
        return;
    }

    PciDevice pci_dev;

    // Try to find HD Audio compatible sound card.
    // Real hardware may have more than one HD Audio device (ex. AMD HD Audio + NVIDIA HD Audio)
    // TO-DO: initialize all HD Audio controllers present in system or find useful one.
    if (!pci_find_hda(&pci_dev)) {
        DEBUG_ERROR("pci_find_hda failed");
        return;
    }

    DEBUG_INFO("hd audio device found at pci bus %d | device %d | function %d",
               pci_dev.bus,
               pci_dev.device,
               pci_dev.function);

    // Enable memory space and bus mastering for sound card device.
    pci_enable_memory_space(&pci_dev);
    pci_enable_bus_mastering(&pci_dev);

    DEBUG_INFO("enabled memory space and bus mastering for hd audio device");

    uint64_t bar0_size = 0;
    uint64_t bar0_base = pci_get_bar(&pci_dev, 0, &bar0_size);

    // Get virtual address of PCI BAR0. Proper MMIO mapping?
    hda_info.base = vmm_map_mmio(bar0_base, bar0_size);
    if (!hda_info.base) {
        DEBUG_ERROR("failed to map mmio");
        return;
    }

    // Initialize HD Audio controller.

    // Controller state is first 8 bits of Global Control register.
    // 0 - In reset state.
    // 1 - In operational state.

    // Try to reset.
    mmio_write16((void*)(hda_info.base + HDA_GLOBAL_CONTROL), HDA_GLOBAL_CONTROL_IN_RESET);

    DEBUG_INFO("waiting for reset");

    // Wait for reset.
    while (mmio_read16((void*)(hda_info.base + HDA_GLOBAL_CONTROL)) & HDA_GLOBAL_CONTROL_IN_OPERATIONAL_STATE) {
        io_wait();
    }

    // Try to set operational state.
    mmio_write16((void*)(hda_info.base + HDA_GLOBAL_CONTROL), HDA_GLOBAL_CONTROL_IN_OPERATIONAL_STATE);

    DEBUG_INFO("waiting for reset");

    // Wait for completion.
    while (!(mmio_read16((void*)(hda_info.base + HDA_GLOBAL_CONTROL)) & HDA_GLOBAL_CONTROL_IN_OPERATIONAL_STATE)) {
        io_wait();
    }

    // Disable all interrupts by writing 0 to all bits.
    // Bit 31 = Interrupt state.
    // 0 - All interrupts are disabled.
    // 1 - All interrupts are enabled.
    mmio_write32((void*)(hda_info.base + HDA_INTERRUPT_CONTROL), 0);

    // Turn off dma position transfer.
    mmio_write64((void*)(hda_info.base + HDA_DMA_POSITION_BASE_ADDRESS), 0);

    // Disable stream synchronization.
    mmio_write32((void*)(hda_info.base + HDA_STREAM_SYNCHRONIZATION), 0);

    // Get output stream address.
    // There are a lot of stream descriptors but we need only output stream. Input stream descriptors always go first.
    // So to get address of output stream descriptor we need to skip input stream descriptors by adding input stream count multiplied by stream descriptor size.

    // Number of input streams is located in global capabilities register at 8-11 bits.
    // So, here we read first 2 bytes (16 bits) of global capabilities and get 8-11 bits.

    // Bit 3-7 = Number of bidirectional streams.
    // Bit 8-11 = Number of input streams.
    // Bit 12-15 = Number of output streams.
    uint16_t input_stream_count = (mmio_read16((void*)(hda_info.base + HDA_GLOBAL_CAPABILITIES)) >> 8) & 0xF;

    // Get first output stream descriptor by adding descriptor size multiplied by input stream count to descriptor base.
    hda_info.output_stream = hda_info.base + HDA_STREAM_DESCRIPTOR_BASE + HDA_STREAM_DESCRIPTOR_SIZE * input_stream_count;

    // Allocate CORB DMA.
    uint64_t corb_size = sizeof(uint32_t) * HDA_CORB_ENTRY_COUNT;
    hda_info.corb_dma = vmm_alloc_dma((corb_size + 4095) / 4096);
    hda_info.corb = (uint32_t*)hda_info.corb_dma.virt;

    if (!hda_info.corb) {
        DEBUG_ERROR("failed to allocate corb");
        return;
    }

    // Allocate RIRB DMA.
    uint64_t rirb_size = sizeof(uint32_t) * HDA_RIRB_ENTRY_COUNT * 2;
    hda_info.rirb_dma = vmm_alloc_dma((rirb_size + 4095) / 4096);
    hda_info.rirb = (uint32_t*)hda_info.rirb_dma.virt;

    if (!hda_info.rirb) {
        DEBUG_ERROR("failed to allocate rirb");
        return;
    }

    // Reset CORB/RIRB positions.
    hda_info.corb_entry = 1;
    hda_info.rirb_entry = 1;

    // Allocate Buffer Descriptor List entries.
    uint64_t bdl_size = sizeof(HdAudioBufferEntry) * HDA_BUFFER_ENTRY_COUNT;
    hda_info.buffer_entries_dma = vmm_alloc_dma((bdl_size + 4095) / 4096);
    hda_info.buffer_entries = (HdAudioBufferEntry*)hda_info.buffer_entries_dma.virt;

    if (!hda_info.buffer_entries) {
        DEBUG_ERROR("failed to allocate buffer descriptor list");
        return;
    }

    // Allocate sound buffers.
    uint64_t sound_buf_size = HDA_BUFFER_ENTRY_SOUND_BUFFER_SIZE * HDA_BUFFER_ENTRY_COUNT;
    hda_info.sound_buffers_dma = vmm_alloc_dma((sound_buf_size + 4095) / 4096);

    if (!hda_info.sound_buffers_dma.virt) {
        DEBUG_ERROR("failed to allocate sound buffers");
        return;
    }

    // Tell sound card where to find CORB and RIRB. Set physical(!!!) addresses of CORB/RIRB.
    mmio_write64((void*)(hda_info.base + HDA_CORB_BASE_ADDRESS), hda_info.corb_dma.phys); // CORB
    mmio_write64((void*)(hda_info.base + HDA_RIRB_BASE_ADDRESS), hda_info.rirb_dma.phys); // RIRB

    // Bit 0 = Number of CORB/RIRB ring entries.
    // 0b00 - 2 entries.
    // 0b01 - 16 entries.
    // 0b10 - 256 entries.

    // Tell sound card how many entries CORB/RIRB have. We assume that every sound card has 256 entries, so set 256 entries.
    mmio_write8((void*)(hda_info.base + HDA_CORB_SIZE), (0b10 << HDA_CORB_SIZE_NUMBER_OF_RING_ENTRIES)); // CORB
    mmio_write8((void*)(hda_info.base + HDA_RIRB_SIZE), (0b10 << HDA_RIRB_SIZE_NUMBER_OF_RING_ENTRIES)); // RIRB

    // Try to reset CORB read pointer.
    mmio_write16((void*)(hda_info.base + HDA_CORB_READ_POINTER), HDA_CORB_READ_POINTER_IN_RESET);

    DEBUG_INFO("waiting for corb read pointer reset");

    // Wait for reset.
    while (!(mmio_read16((void*)(hda_info.base + HDA_CORB_READ_POINTER)) & HDA_CORB_READ_POINTER_IN_RESET)) {
        io_wait();
    }

    // Clear read pointer.
    mmio_write16((void*)(hda_info.base + HDA_CORB_READ_POINTER), HDA_CORB_READ_POINTER_CLEAR);

    DEBUG_INFO("waiting for corb read pointer clear");

    // Wait for reset.
    while (mmio_read16((void*)(hda_info.base + HDA_CORB_READ_POINTER)) & HDA_CORB_READ_POINTER_IN_RESET) {
        io_wait();
    }

    // Set CORB write pointer.
    mmio_write16((void*)(hda_info.base + HDA_CORB_WRITE_POINTER), 0);

    // Reset RIRB write pointer.
    mmio_write16((void*)(hda_info.base + HDA_RIRB_WRITE_POINTER), HDA_RIRB_WRITE_POINTER_IN_RESET);

    // Disable RIRB interrupts.
    mmio_write16((void*)(hda_info.base + HDA_RIRB_RESPONSE_INTERRUPT_COUNT), 0);

    // Bit 1 = CORB/RIRB stopped/running.
    // 0 - stopped.
    // 1 - running.

    // Tell sound card that we are using CORB/RIRB.
    mmio_write8((void*)(hda_info.base + HDA_CORB_CONTROL), HDA_CORB_CONTROL_STATUS_RUNNING); // CORB
    mmio_write8((void*)(hda_info.base + HDA_RIRB_CONTROL), HDA_RIRB_CONTROL_STATUS_RUNNING); // RIRB

    // Sound card initialization has been finished.
    // Now we need to find valid codec.

    DEBUG_INFO("searching for codec");

    // Reset codec and AFG
    hda_info.codec = HDA_INVALID;
    hda_info.afg.node = HDA_INVALID;

    // Check first 8 codecs. In almost every case first usable codec is at 0, but lets scan first 8 just in case.
    for (uint32_t codec = 0; codec < 8; codec++) {
        // Send command to root node of each codec id.
        uint32_t codec_present = hda_send_command(codec, 0, HDA_VERB_GET_PARAMETER, 0);

        // In this case, hda_send_command returns 0 if codec is not present and any other number if codec is present.
        if (codec_present) {
            DEBUG_INFO("found codec at %d", codec);

            hda_info.codec = codec;
            break;
        }
    }

    // Check if we found any usable codec.
    if (hda_info.codec == HDA_INVALID) {
        DEBUG_ERROR("failed to find codec.");
        return;
    }

    // Proceed with searching for Audio Function Group in codec.

    DEBUG_INFO("searching for afg node");

    // Get function group count in root node of codec.
    uint32_t node_count = hda_send_command(hda_info.codec, 0, HDA_VERB_GET_PARAMETER, HDA_NODE_PARAMETER_NODE_COUNT);

    // Search for Audio Function Group.
    uint32_t first_node = (node_count >> 16) & 0xFF;
    uint32_t last_node = first_node + (node_count & 0xFF);
    for (uint32_t node = first_node; node < last_node; node++) {
        // Check function group type of each node if it's AFG.
        if ((hda_send_command(hda_info.codec, node, HDA_VERB_GET_PARAMETER, HDA_NODE_PARAMETER_FUNCTION_GROUP_TYPE) & 0x7F) == 0x1) {
            DEBUG_INFO("found afg at %d", node);

            // Set AFG node and break from cycle.
            hda_info.afg.init(node, HDA_WIDGET_AFG, HDA_INVALID, HDA_INVALID);
            break;
        }
    }

    if (hda_info.afg.node == HDA_INVALID) {
        DEBUG_ERROR("failed to find afg node.");
        return;
    }

    // Set power state for AFG.
    hda_power_on_node(&hda_info.afg);

    // Reset all nodes inside codec AFG.
    hda_send_command(hda_info.codec, hda_info.afg.node, HDA_VERB_AFG_NODE_RESET, 0);

    // Wait for node reset.
    for (uint32_t i = 0; i < 100; i++)
        io_wait();

    // Collect AFG capabilities.
    hda_info.afg.supported_rates = hda_send_command(hda_info.codec, hda_info.afg.node, HDA_VERB_GET_PARAMETER, HDA_NODE_PARAMETER_SUPPORTED_PCM_RATES);
    hda_info.afg.supported_formats = hda_send_command(hda_info.codec, hda_info.afg.node, HDA_VERB_GET_PARAMETER, HDA_NODE_PARAMETER_SUPPORTED_FORMATS);

    hda_info.afg.output_amplifier_capabilities = hda_send_command(hda_info.codec, hda_info.afg.node, HDA_VERB_GET_PARAMETER, HDA_NODE_PARAMETER_OUTPUT_AMPLIFIER_CAPABILITIES);

    // AFG is initialized now. Initialize AFG nodes.

    // Get node count from AFG.
    node_count = hda_send_command(hda_info.codec, hda_info.afg.node, HDA_VERB_GET_PARAMETER, HDA_NODE_PARAMETER_NODE_COUNT);

    // Iterate all AFG nodes and find useful widgets and pins.
    first_node = (node_count >> 16) & 0xFF;
    last_node = first_node + (node_count & 0xFF);
    for (uint32_t node = first_node; node < last_node; node++) {
        // Get pointer to this node.
        HdAudioNode* current_node = &hda_info.nodes[node];

        // Get audio widget capabilities and node widget type.
        uint8_t node_type = (hda_send_command(hda_info.codec, node, HDA_VERB_GET_PARAMETER, HDA_NODE_PARAMETER_AUDIO_WIDGET_CAPABILITIES) >> 20) & 0xF;

        // Get parent node and type of parent node of this pin.
        uint32_t parent_node = hda_get_node_connection_entry(current_node, 0);
        uint32_t parent_node_type = (hda_send_command(hda_info.codec, node, HDA_VERB_GET_PARAMETER, HDA_NODE_PARAMETER_AUDIO_WIDGET_CAPABILITIES) >> 20) & 0xF;

        // Initialize current node.
        current_node->init(node, node_type, parent_node, parent_node_type);

        if (node_type == HDA_WIDGET_AUDIO_OUTPUT) {
            DEBUG_INFO("found audio output widget at %d of %d", node, current_node->parent_node);

            // Initialize this output.
            hda_init_output(current_node);
        }
        else if (node_type == HDA_WIDGET_AUDIO_MIXER) {
            DEBUG_INFO("found audio mixer widget at %d", node);

            // Initialize this mixer.
            hda_init_mixer(current_node);
        }
        else if (node_type == HDA_WIDGET_PIN_COMPLEX) {
            // This node is pin. Get type of this pin node with pin widget configuration command.
            uint8_t pin_type = (hda_send_command(hda_info.codec, node, HDA_VERB_GET_PIN_WIDGET_CONFIGURATION, 0) >> 20) & 0xF;
            if (pin_type == HDA_PIN_LINE_OUT) {
                // Initialize line out pin. Collect needed capabilities.
                DEBUG_INFO("found line out pin widget at %d", node);

                // Add this pin to array and initialize it.
                hda_init_pin(current_node);
            }
            else if (pin_type == HDA_PIN_HEADPHONE_OUT) {
                // Initialize headphone out pin. Collect needed capabilities.
                DEBUG_INFO("found headphone out pin widget at %d", node);

                // Add this pin to array and initialize it.
                hda_init_pin(current_node);
            }
            else {
                // Don't enable power for not needed pins.
            }
        }

        hda_info.node_count++;
    }

    // Init complete.
    hda_info.is_initialized = true;

    // Reset everything.
    hda_reset();

    // Set default volume to 100%
    hda_set_volume(100);

    DEBUG_INFO("init completed");
}

// Credits: BleskOS HDA driver.
// TO-DO: Refactor. Revisit this part later.
uint16_t hda_get_node_connection_entry(HdAudioNode* node, uint32_t connection_entry_number) {
    // Read connection list length.
    uint32_t list_length = hda_send_command(hda_info.codec, node->node, HDA_VERB_GET_PARAMETER, HDA_NODE_PARAMETER_CONNECTION_LIST_LENGTH);

    // Check if entry number in bounds.
    if (connection_entry_number >= (list_length & 0x7F)) {
        return 0;
    }

    // Short form?
    if(!(list_length & 0x80)) {
        return ((hda_send_command(hda_info.codec, node->node, HDA_VERB_GET_CONNECTION_LIST_ENTRY, ((connection_entry_number/4)*4)) >> ((connection_entry_number%4)*8)) & 0xFF);
    }

    return ((hda_send_command(hda_info.codec, node->node, HDA_VERB_GET_CONNECTION_LIST_ENTRY, ((connection_entry_number/2)*2)) >> ((connection_entry_number%2)*16)) & 0xFFFF);
}

// Set power state for node.
void hda_power_on_node(HdAudioNode* node) {
    // Set power state for this pin. 0 - full power.
    hda_send_command(hda_info.codec, node->node, HDA_VERB_SET_POWER_STATE, 0);

    // Wait for powering on.
    for (uint32_t i = 0; i < 1000; i++)
        io_wait();
}

// Initialize pin widget.
void hda_init_pin(HdAudioNode* node) {
    // Check if node widget type is pin complex.
    if (node->node_type != HDA_WIDGET_PIN_COMPLEX) {
        DEBUG_WARN("trying to initialize non pin widget");
        return;
    }

    // Set power state for this pin.
    hda_power_on_node(node);

    // Enable pin.
    hda_send_command(hda_info.codec, node->node, HDA_VERB_SET_PIN_WIDGET_CONTROL, (hda_send_command(hda_info.codec, node->node, 0xF07, 0) | 0x80 | 0x40));

    // Collect node capabilities.
    node->supported_rates = hda_send_command(hda_info.codec, node->node, HDA_VERB_GET_PARAMETER, HDA_NODE_PARAMETER_SUPPORTED_PCM_RATES);
    node->supported_formats = hda_send_command(hda_info.codec, node->node, HDA_VERB_GET_PARAMETER, HDA_NODE_PARAMETER_SUPPORTED_FORMATS);

    node->output_amplifier_capabilities = hda_send_command(hda_info.codec, node->node, HDA_VERB_GET_PARAMETER, HDA_NODE_PARAMETER_OUTPUT_AMPLIFIER_CAPABILITIES);

    // Enable EAPD.
    hda_send_command(hda_info.codec, node->node, HDA_VERB_SET_EAPD, 0x6);

    for (uint32_t i = 0; i < 1000; i++)
        io_wait();

    // Mute pin by default. We will set volume only for needed ones.
    hda_set_node_volume(node, 0);
}

// Initialize audio mixer widget.
void hda_init_mixer(HdAudioNode* node) {
    // Check if node widget type is mixer.
    if (node->node_type != HDA_WIDGET_AUDIO_MIXER) {
        DEBUG_WARN("trying to initialize non audio output widget");
        return;
    }

    // Set power state for this widget.
    hda_power_on_node(node);

    // Collect node capabilities.
    node->supported_rates = hda_send_command(hda_info.codec, node->node, HDA_VERB_GET_PARAMETER, HDA_NODE_PARAMETER_SUPPORTED_PCM_RATES);
    node->supported_formats = hda_send_command(hda_info.codec, node->node, HDA_VERB_GET_PARAMETER, HDA_NODE_PARAMETER_SUPPORTED_FORMATS);

    node->output_amplifier_capabilities = hda_send_command(hda_info.codec, node->node, HDA_VERB_GET_PARAMETER, HDA_NODE_PARAMETER_OUTPUT_AMPLIFIER_CAPABILITIES);

    for (uint32_t i = 0; i < 1000; i++)
        io_wait();

    // Mute mixer by default. We will set volume only for needed ones.
    hda_set_node_volume(node, 0);
}

// Initialize audio output widget.
void hda_init_output(HdAudioNode* node) {
    // Check if node widget type is output.
    if (node->node_type != HDA_WIDGET_AUDIO_OUTPUT) {
        DEBUG_WARN("trying to initialize non audio output widget");
        return;
    }

    // Set power state for this widget.
    hda_power_on_node(node);

    // Collect node capabilities.
    node->supported_rates = hda_send_command(hda_info.codec, node->node, HDA_VERB_GET_PARAMETER, HDA_NODE_PARAMETER_SUPPORTED_PCM_RATES);
    node->supported_formats = hda_send_command(hda_info.codec, node->node, HDA_VERB_GET_PARAMETER, HDA_NODE_PARAMETER_SUPPORTED_FORMATS);

    node->output_amplifier_capabilities = hda_send_command(hda_info.codec, node->node, HDA_VERB_GET_PARAMETER, HDA_NODE_PARAMETER_OUTPUT_AMPLIFIER_CAPABILITIES);

    // Connect to output stream.
    hda_send_command(hda_info.codec, node->node, HDA_VERB_SET_CONVERTER_STREAM, 0x10);

    // Enable EAPD.
    hda_send_command(hda_info.codec, node->node, HDA_VERB_SET_EAPD, 0x6);

    for (uint32_t i = 0; i < 1000; i++)
        io_wait();

    // Mute audio output by default. We will set volume only for needed ones.
    hda_set_node_volume(node, 0);
}

// Clean buffers and reset flags.
void hda_reset() {
    if (!hda_info.is_initialized) {
        DEBUG_ERROR("hd audio device is not initialized");
        return;
    }

    DEBUG_INFO("cleaning playback info");

    // Reset buffer info.
    hda_info.current_buffer_entry = 0;
    hda_info.buffer_entry_offset = 0;

    // Reset flags.
    hda_info.is_playing = false;
    hda_info.is_paused = false;

    hda_info.played_bytes = 0;

    // Clean buffer entries and sound buffer.
    memset((void*)hda_info.buffer_entries_dma.virt, 0, hda_info.buffer_entries_dma.size);
    memset((void*)hda_info.sound_buffers_dma.virt, 0, hda_info.sound_buffers_dma.size);

    // Do not free this one unless asked! It's pointer to file system file data!
    if (hda_info.free_sound_data_on_stop) {
        free(hda_info.sound_data);
        hda_info.free_sound_data_on_stop = false;
    }

    hda_info.sound_data = nullptr;
    hda_info.sound_data_size = 0;
}

// Send verb via CORB/RIRB buffers.
uint32_t hda_send_command(uint32_t codec, uint32_t node, uint32_t verb, uint32_t command) {
    // CORB entry structure.
    // Bit 0-7 - Data.
    // Bit 8-19 - Command (Verb).
    // Bit 20-27 - Node.
    // Bit 28-31 - Codec.
    hda_info.corb[hda_info.corb_entry] = (codec << HDA_NODE_COMMAND_CODEC) | (node << HDA_NODE_COMMAND_NODE_INDEX) | (verb << HDA_NODE_COMMAND_COMMAND) | (command << HDA_NODE_COMMAND_DATA);

    // Update current CORB write pointergj.
    mmio_write16((void*)(hda_info.base + HDA_CORB_WRITE_POINTER), hda_info.corb_entry);

    // Generates too much useless logs but can be uncommented if needed.
    // DEBUG_INFO("waiting for response");

    // Wait for response. May cause dead lock in some scenarios but having timeout is also bad since some machines may not respond in time.
    while (mmio_read16((void*)(hda_info.base + HDA_RIRB_WRITE_POINTER)) != hda_info.corb_entry) {
        io_wait();
    }

    // Read response.
    uint32_t response = hda_info.rirb[hda_info.rirb_entry * 2];

    // Move to next CORB entry and make sure it does not exceed number of entries.
    hda_info.corb_entry++;
    if (hda_info.corb_entry >= HDA_CORB_ENTRY_COUNT) {
        hda_info.corb_entry = 0;
    }

    // Move to next RIRB entry and make sure it does not exceed number of entries.
    hda_info.rirb_entry++;
    if (hda_info.rirb_entry >= HDA_RIRB_ENTRY_COUNT) {
        hda_info.rirb_entry = 0;
    }

    return response;
}

// Set node volume.
// Credits: BleskOS HDA driver.
void hda_set_node_volume(HdAudioNode* node, uint32_t volume) {
    uint32_t payload = 0x3000;
    payload |= 0x8000;

    // In some cases audio output and pin widgets don't have their own amplifier capabilities.
    // So, if node does not have output amplifier capability, use ones from AFG or parent node.
    uint32_t output_amplifier_capabilities = node->output_amplifier_capabilities;
    if (!output_amplifier_capabilities && node->parent_node) {
        output_amplifier_capabilities = hda_info.nodes[node->parent_node].output_amplifier_capabilities;
    }

    if (!output_amplifier_capabilities) {
        output_amplifier_capabilities = hda_info.afg.output_amplifier_capabilities;
    }

    if (!output_amplifier_capabilities) {
        DEBUG_WARN("output amp capabilities are 0 at node %d | %d", node->node, node->node_type);
        return;
    }

    if (volume == 0 && output_amplifier_capabilities & 0x80000000) {
        payload |= 0x80;
    }
    else {
        payload |= (volume * ((output_amplifier_capabilities >> 8) & 0x7F) / 100);
    }

    // Set gain.
    hda_send_command(hda_info.codec, node->node, HDA_VERB_SET_AMPLIFIER_GAIN, payload);

    DEBUG_INFO("set node %d | %d volume to %d (amp capabilities: %p)", node->node, node->node_type, volume, output_amplifier_capabilities);
}

// Set current node volume.
void hda_set_volume(uint8_t volume) {
    if (!hda_info.is_initialized) {
        DEBUG_ERROR("hd audio device is not initialized");
        return;
    }

    hda_info.sound_volume = volume;

    // Iterate all nodes and set volume for each pin node.
    // Proven to work on real hardware. VirtualBox and other VMs may have problems with pin volume.
    for (uint32_t node = 0; node < HDA_MAX_AFG_NODES; node++) {
        HdAudioNode* current_node = &hda_info.nodes[node];
        if (current_node->node_type == HDA_WIDGET_AUDIO_OUTPUT || current_node->node_type == HDA_WIDGET_PIN_COMPLEX)
            hda_set_node_volume(current_node, volume);
    }

    DEBUG_INFO("set volume to %d", hda_info.sound_volume);
}

// Get node volume set previously.
uint8_t hda_get_volume() {
    return hda_info.sound_volume;
}

// Set channel count for output sound data.
void hda_set_channels(uint8_t channels) {
    hda_info.channels = channels;
}

// Set bits per sample for output sound data.
void hda_set_bits_per_sample(uint8_t bits_per_sample) {
    hda_info.bits_per_sample = bits_per_sample;
}

// Set sample rate for output sound data.
void hda_set_sample_rate(uint32_t sample_rate) {
    hda_info.sample_rate = sample_rate;
}

// Construct 16-bit sound format from sample rate, channel count and bits per sample.
// Used for setting stream format.
// Credits: BleskOS HDA driver.
uint16_t hda_return_sound_data_format(uint32_t sample_rate, uint32_t channels, uint32_t bits_per_sample) {
    uint16_t data_format = (channels - 1);

    // Bits per sample.
    if (bits_per_sample==16) {
        data_format |= (0b001 << 4);
    }
    else if (bits_per_sample==20) {
        data_format |= (0b010 << 4);
    }
    else if (bits_per_sample==24) {
        data_format |= (0b011 << 4);
    }
    else if (bits_per_sample==32) {
        data_format |= (0b100 << 4);
    }

    // Sample rate.
    if (sample_rate==48000) {
        data_format |= (0b0000000 << 8);
    }
    else if (sample_rate==44100) {
        data_format |= (0b1000000 << 8);
    }
    else if (sample_rate==32000) {
        data_format |= (0b0001010 << 8);
    }
    else if (sample_rate==22050) {
        data_format |= (0b1000001 << 8);
    }
    else if (sample_rate==16000) {
        data_format |= (0b0000010 << 8);
    }
    else if (sample_rate==11025) {
        data_format |= (0b1000011 << 8);
    }
    else if (sample_rate==8000) {
        data_format |= (0b0000101 << 8);
    }
    else if (sample_rate==88200) {
        data_format |= (0b1001000 << 8);
    }
    else if (sample_rate==96000) {
        data_format |= (0b0001000 << 8);
    }
    else if (sample_rate==176400) {
        data_format |= (0b1011000 << 8);
    }
    else if (sample_rate==192000) {
        data_format |= (0b0011000 << 8);
    }

    return data_format;
}

// Play PCM byte array.
void hda_play(uint8_t* data, uint32_t size) {
    if (!hda_info.is_initialized) {
        DEBUG_INFO("hd audio device is not initialized");
        return;
    }

    // Do not play if sound card is already busy. In future we may add sound mixing.
    if (hda_info.is_playing) {
        DEBUG_INFO("already playing! stop current playback before playing next sound");
        return;
    }

    // Reset stream registers.
    mmio_write8((void*)(hda_info.output_stream + HDA_STREAM_DESCRIPTOR_STREAM_CONTROL_1), HDA_STREAM_CONTROL_STREAM_IN_RESET);

    DEBUG_INFO("waiting for stream reset");

    // Wait for reset.
    while (!(mmio_read8((void*)(hda_info.output_stream + HDA_STREAM_DESCRIPTOR_STREAM_CONTROL_1)) & HDA_STREAM_CONTROL_STREAM_IN_RESET)) {
        io_wait();
    }

    // Stop stream.
    mmio_write8((void*)(hda_info.output_stream + HDA_STREAM_DESCRIPTOR_STREAM_CONTROL_1), HDA_STREAM_CONTROL_STREAM_STOPPED);

    DEBUG_INFO("waiting for stream to stop");

    // Wait for stop.
    while (mmio_read8((void*)(hda_info.output_stream + HDA_STREAM_DESCRIPTOR_STREAM_CONTROL_1)) & HDA_STREAM_CONTROL_STREAM_IN_RESET) {
        io_wait();
    }

    DEBUG_INFO("playing sound data ptr: %p | data size: %d", data, size);

    // Set sound data source.
    hda_info.sound_data = data;
    hda_info.sound_data_size = size;

    // Copy new buffer data (bounded to avoid reading past source)
    size_t copy_size = (size < hda_info.sound_buffers_dma.size) ? size : hda_info.sound_buffers_dma.size;
    memcpy((void*)hda_info.sound_buffers_dma.virt, data, copy_size);
    // Zero remaining buffer if source was smaller
    if (copy_size < hda_info.sound_buffers_dma.size) {
        memset((void*)(hda_info.sound_buffers_dma.virt + copy_size), 0, hda_info.sound_buffers_dma.size - copy_size);
    }

    DEBUG_INFO("filling buffer entries");

    // Fill entries.
    uint64_t mem_offset = 0;
    for(uint32_t i = 0; i < HDA_BUFFER_ENTRY_COUNT; i++) {
        // Fill current entry.
        hda_info.buffer_entries[i].buffer = hda_info.sound_buffers_dma.phys + mem_offset;

        // Set buffer size.
        hda_info.buffer_entries[i].buffer_size = HDA_BUFFER_ENTRY_SOUND_BUFFER_SIZE;

        // Add offset to read next sound data for next buffer entry. We don't want to listen to same buffer 24/7.
        mem_offset += HDA_BUFFER_ENTRY_SOUND_BUFFER_SIZE;
    }

    // Write buffer descriptor list address and total buffer length.
    mmio_write64((void*)(hda_info.output_stream + HDA_STREAM_DESCRIPTOR_BDL_BASE_ADDRESS), hda_info.buffer_entries_dma.phys);
    mmio_write32((void*)(hda_info.output_stream + HDA_STREAM_DESCRIPTOR_RING_BUFFER_LENGTH), HDA_BUFFER_ENTRY_SOUND_BUFFER_SIZE * HDA_BUFFER_ENTRY_COUNT);

    // Write last valid entry of buffer ( entries count - 1!!!! ).
    mmio_write16((void*)(hda_info.output_stream + HDA_STREAM_DESCRIPTOR_LAST_VALID_INDEX), HDA_BUFFER_ENTRY_COUNT - 1);

    // Get sound format from sample rate, channels and bps.
    uint16_t sound_format = hda_return_sound_data_format(hda_info.sample_rate, hda_info.channels, hda_info.bits_per_sample);

    // Set stream format.
    mmio_write16((void*)(hda_info.output_stream + HDA_STREAM_DESCRIPTOR_STREAM_FORMAT), sound_format);

    // Set audio output nodes format.
    for (uint32_t node = 0; node < HDA_MAX_AFG_NODES; node++) {
        HdAudioNode* current_node = &hda_info.nodes[node];
        if (current_node->node_type == HDA_WIDGET_AUDIO_OUTPUT) {
            hda_send_command(hda_info.codec, current_node->node, HDA_VERB_SET_STREAM_FORMAT, sound_format);
            io_wait();
        }
    }

    io_wait();

    // Start stream.
    mmio_write8((void*)(hda_info.output_stream + HDA_STREAM_DESCRIPTOR_STREAM_CONTROL_2), 0x14);
    mmio_write8((void*)(hda_info.output_stream + HDA_STREAM_DESCRIPTOR_STREAM_CONTROL_1), HDA_STREAM_CONTROL_STREAM_RUNNING);

    DEBUG_INFO("started playback");

    // Let everyone know audio is playing.
    hda_info.is_paused = false;
    hda_info.is_playing = true;
}

// Resume playback if we played something before.
void hda_resume() {
    if (!hda_info.is_initialized) {
        DEBUG_ERROR("hd audio device is not initialized");
        return;
    }

    // Nothing to resume?
    if (!hda_info.is_playing) {
        DEBUG_WARN("trying to resume playback, but nothing is played!");
        return;
    }

    mmio_write8((void*)(hda_info.output_stream + HDA_STREAM_DESCRIPTOR_STREAM_CONTROL_1), HDA_STREAM_CONTROL_STREAM_RUNNING);

    hda_info.is_paused = false;
}

// Pause playback.
void hda_pause() {
    if (!hda_info.is_initialized) {
        DEBUG_ERROR("hd audio device is not initialized");
        return;
    }

    // Nothing to pause?
    if (!hda_info.is_playing) {
        DEBUG_WARN("trying to pause playback, but nothing is played!");
        return;
    }

    mmio_write8((void*)(hda_info.output_stream + HDA_STREAM_DESCRIPTOR_STREAM_CONTROL_1), HDA_STREAM_CONTROL_STREAM_STOPPED);

    hda_info.is_paused = true;
}

// Full stop.
void hda_stop() {
    if (!hda_info.is_initialized) {
        DEBUG_ERROR("hd audio device is not initialized");
        return;
    }

    // Nothing to stop?
    if (!hda_info.is_playing) {
        DEBUG_WARN("trying to stop playback, but nothing is played!");
        return;
    }

    DEBUG_INFO("trying to reset stream");

    // Stop stream.
    mmio_write8((void*)(hda_info.output_stream + HDA_STREAM_DESCRIPTOR_STREAM_CONTROL_1), HDA_STREAM_CONTROL_STREAM_STOPPED);

    DEBUG_INFO("waiting for stop");

    // Wait for stop.
    while (mmio_read8((void*)(hda_info.output_stream + HDA_STREAM_DESCRIPTOR_STREAM_CONTROL_1)) & HDA_STREAM_CONTROL_STREAM_STOPPED) {
        io_wait();
    }

    // Reset stream registers.
    mmio_write8((void*)(hda_info.output_stream + HDA_STREAM_DESCRIPTOR_STREAM_CONTROL_1), HDA_STREAM_CONTROL_STREAM_IN_RESET);

    DEBUG_INFO("waiting for reset");

    // Wait for reset.
    while (!(mmio_read8((void*)(hda_info.output_stream + HDA_STREAM_DESCRIPTOR_STREAM_CONTROL_1)) & HDA_STREAM_CONTROL_STREAM_IN_RESET)) {
        io_wait();
    }

    // Reset playback info.
    hda_reset();

    DEBUG_INFO("stopped playback");
}

void hda_poll() {
    // Check if initialized and playing.
    if (!hda_info.is_initialized || !hda_info.is_playing || hda_info.is_paused) {
        return;
    }

    // Do not play further than source sound data.
    if (hda_info.played_bytes >= hda_info.sound_data_size) {
        hda_stop();
        return;
    }

    // Get current stream position (between buffer entries) and buffer entry.
    uint32_t stream_pos = hda_get_stream_position();
    uint32_t stream_curr_buffer_entry = stream_pos / HDA_BUFFER_ENTRY_SOUND_BUFFER_SIZE;

    // Reset current buffer entry and increase offset if sound card moved back to first entry.
    if (stream_curr_buffer_entry == 0 && hda_info.current_buffer_entry > 0) {
        hda_info.current_buffer_entry = 0;
        hda_info.buffer_entry_offset++;

        // Calculate source offset and bounds-check before copying
        uint32_t src_offset = HDA_BUFFER_ENTRY_SOUND_BUFFER_SIZE * (HDA_BUFFER_ENTRY_COUNT * hda_info.buffer_entry_offset + (HDA_BUFFER_ENTRY_COUNT - 1));
        void* dst = (void*)(hda_info.sound_buffers_dma.virt + (HDA_BUFFER_ENTRY_SOUND_BUFFER_SIZE * (HDA_BUFFER_ENTRY_COUNT - 1)));
        if (src_offset < hda_info.sound_data_size) {
            uint32_t avail = hda_info.sound_data_size - src_offset;
            uint32_t copy_len = (avail < HDA_BUFFER_ENTRY_SOUND_BUFFER_SIZE) ? avail : HDA_BUFFER_ENTRY_SOUND_BUFFER_SIZE;
            memcpy(dst, hda_info.sound_data + src_offset, copy_len);
            // Zero remainder if partial
            if (copy_len < HDA_BUFFER_ENTRY_SOUND_BUFFER_SIZE) {
                memset((void*)((uint8_t*)dst + copy_len), 0, HDA_BUFFER_ENTRY_SOUND_BUFFER_SIZE - copy_len);
            }
        } else {
            // Past end of data, zero the buffer
            memset(dst, 0, HDA_BUFFER_ENTRY_SOUND_BUFFER_SIZE);
        }
    }

    // Refill previous buffer with fresh data.
    if (stream_pos > (HDA_BUFFER_ENTRY_SOUND_BUFFER_SIZE * (hda_info.current_buffer_entry + 1))) {
        // Calculate source offset and bounds-check before copying
        uint32_t src_offset = HDA_BUFFER_ENTRY_SOUND_BUFFER_SIZE * (HDA_BUFFER_ENTRY_COUNT * (hda_info.buffer_entry_offset + 1) + hda_info.current_buffer_entry);
        void* dst = (void*)(hda_info.sound_buffers_dma.virt + (HDA_BUFFER_ENTRY_SOUND_BUFFER_SIZE * hda_info.current_buffer_entry));
        if (src_offset < hda_info.sound_data_size) {
            uint32_t avail = hda_info.sound_data_size - src_offset;
            uint32_t copy_len = (avail < HDA_BUFFER_ENTRY_SOUND_BUFFER_SIZE) ? avail : HDA_BUFFER_ENTRY_SOUND_BUFFER_SIZE;
            memcpy(dst, hda_info.sound_data + src_offset, copy_len);
            // Zero remainder if partial
            if (copy_len < HDA_BUFFER_ENTRY_SOUND_BUFFER_SIZE) {
                memset((void*)((uint8_t*)dst + copy_len), 0, HDA_BUFFER_ENTRY_SOUND_BUFFER_SIZE - copy_len);
            }
        } else {
            // Past end of data, zero the buffer
            memset(dst, 0, HDA_BUFFER_ENTRY_SOUND_BUFFER_SIZE);
        }

        // Now we can move to next entry.
        hda_info.current_buffer_entry++;
    }

    hda_info.played_bytes = (hda_info.buffer_entry_offset * HDA_BUFFER_ENTRY_COUNT * HDA_BUFFER_ENTRY_SOUND_BUFFER_SIZE) + stream_pos;
}

uint32_t hda_get_played_bytes() {
    return hda_info.played_bytes;
}

// Get stream position.
uint32_t hda_get_stream_position() {
    if (!hda_info.is_initialized) {
        DEBUG_ERROR("hd audio device is not initialized");
        return 0;
    }

    // Nothing is playing?
    if (!hda_info.is_playing) {
        DEBUG_WARN("nothing is playing");
        return 0;
    }

    return mmio_read32((void*)(hda_info.output_stream + HDA_STREAM_DESCRIPTOR_BUFFER_ENTRY_POSITION));
}
