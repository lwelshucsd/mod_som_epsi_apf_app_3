/*
 * mod_som_food_bar.c
 *
 *  Created on: Apr 3, 2020
 *      Author: snguyen
 */
#include <efe_obp/mod_som_efe_obp.h>
#include "mod_som_io.h"
#include "mod_som_priv.h"

#include <math.h>

#ifdef  RTOS_MODULE_COMMON_SHELL_AVAIL
#include <efe_obp/mod_som_efe_obp_cmd.h>
#endif
#ifdef MOD_SOM_SETTINGS_EN
  #include <mod_som_settings.h>
#endif

#ifdef MOD_SOM_SBE41_EN
  #include <mod_som_sbe41.h>
#endif


//MHA
#include <mod_som_calendar.h> //MHA add calendar .h so we have access to the calendar settings pointer for the poweron_offset.

//MHA calendar
mod_som_calendar_settings_t mod_som_calendar_settings;


//PRIVATE DEFINE
#define MOD_SOM_EFE_OBP_DEFAULT_SIZE_LARGEST_BLOCK 128U
#define MOD_SOM_EFE_OBP_MSG_QUEUE_COUNT 64U

//PRIVATE FUNCTIONS
/*******************************************************************************
 * @brief
 *   producer task function
 *
 *   convert cnt to volt
 *   compute the frequency spectra
 *   compute nu and kappa
 *   convert frequency to wavenumber
 *
 * @return
 *   MOD_SOM_STATUS_OK if initialization goes well
 *   or otherwise
 ******************************************************************************/
static void mod_som_efe_obp_fill_segment_task_f();
static void mod_som_efe_obp_cpt_spectra_task_f();
static void mod_som_efe_obp_cpt_dissrate_task_f();

static void mod_som_efe_obp_consumer_task_f(void  *p_arg);
mod_som_status_t mod_som_efe_obp_cnt2volt_f(uint8_t * efe_sample,
                                            float   * local_efeobp_efe_temp_ptr,
                                            float   * local_efeobp_efe_shear_ptr,
                                            float   * local_efeobp_efe_accel_ptr);

//PRIVATE VARIABLES

//sleeptimer status
sl_status_t mystatus;

// Fill segment
static CPU_STK efe_obp_fill_segment_task_stk[MOD_SOM_EFE_OBP_FILL_SEGMENT_TASK_STK_SIZE];
static OS_TCB  efe_obp_fill_segment_task_tcb;

// compute spectra
static CPU_STK efe_obp_cpt_spectra_task_stk[MOD_SOM_EFE_OBP_FILL_SEGMENT_TASK_STK_SIZE];
static OS_TCB  efe_obp_cpt_spectra_task_tcb;

// compute dissrate
static CPU_STK efe_obp_cpt_dissrate_task_stk[MOD_SOM_EFE_OBP_FILL_SEGMENT_TASK_STK_SIZE];
static OS_TCB  efe_obp_cpt_dissrate_task_tcb;


// Data consumer
static CPU_STK efe_obp_consumer_task_stk[MOD_SOM_EFE_OBP_CONSUMER_TASK_STK_SIZE];
static OS_TCB  efe_obp_consumer_task_tcb;


mod_som_efe_obp_ptr_t mod_som_efe_obp_ptr;

mod_som_sbe41_ptr_t local_sbe41_ptr;


/*******************************************************************************
 * @brief
 *   Initialize efeobp, if shell is available, then the command table is added
 *
 * @return
 *   MOD_SOM_STATUS_OK if initialization goes well
 *   or otherwise
 ******************************************************************************/
mod_som_status_t mod_som_efe_obp_init_f(){
    mod_som_status_t status;
    RTOS_ERR  err;

#ifdef  RTOS_MODULE_COMMON_SHELL_AVAIL
    status = mod_som_efe_obp_init_cmd_f();
    if(status != MOD_SOM_STATUS_OK)
        return mod_som_efe_obp_encode_status_f(MOD_SOM_EFE_OBP_STATUS_FAIL_INIT_CMD);
#endif


    //ALB allocate memory for the module_ptr.
    //ALB The module_ptr is also the "scope" of the runtime_ptr
    //ALB but the module_ptr also contains the settings_ptr and the config_ptr
    //ALB The settings_ptr an config_ptr should allocated and defined
    //ALB during the module initialization
    mod_som_efe_obp_ptr = (mod_som_efe_obp_ptr_t)Mem_SegAlloc(
        "MOD SOM EFE OBP RUNTIME Memory",DEF_NULL,
        sizeof(mod_som_efe_obp_t),
        &err);

    //SN Check error code
    //ALB WARNING: The following line hangs in a while loop -
    //ALB WARNING: - if the previous allocation fails.
    //ALB TODO change return -1 to return the an appropriate error code.
    APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);
    if(mod_som_efe_obp_ptr==DEF_NULL){
      printf("EFE OBP not initialized\n");
      return -1;
    }

    //ALB Initialize the runtime flag module_ptr->initialized_flag to false.
    //ALB It will be set to true once the module is initialized at the
    //ALB end of mod_som_efe_init_f().
    mod_som_efe_obp_ptr->initialized_flag = false;

    // ALB allocate memory for the settings_ptr.
    // ALB WARNING: The setup pointer CAN NOT have pointers inside.
    status |= mod_som_efe_obp_allocate_settings_ptr_f();
    if (status!=MOD_SOM_STATUS_OK){
      printf("EFE not initialized\n");
      return status;
    }



    // ALB checking if a previous EFE OBP setup exist, from the setup module
    //ALB (i.e. setup file or UserData setup)
  #ifdef MOD_SOM_SETTINGS_EN
      mod_som_settings_struct_ptr_t local_settings_ptr=
                                              mod_som_settings_get_settings_f();
      mod_som_efe_obp_ptr->settings_ptr=
                                  &local_settings_ptr->mod_som_efe_obp_settings;
  #else
      mod_som_efe_obp_ptr->settings_ptr->initialize_flag=false;
  #endif

    // ALB If no pre-existing settings, use the default settings
      if (!mod_som_efe_obp_ptr->settings_ptr->initialize_flag){
        // initialize the setup structure.
        status |= mod_som_efe_obp_default_settings_f(
                                             mod_som_efe_obp_ptr->settings_ptr);
        if (status!=MOD_SOM_STATUS_OK){
          printf("EFE OBP not initialized\n");
          return status;
        }
      }

      //ALB Allocate memory for the config pointer,
      //ALB using the settings_ptr variable
      status |= mod_som_efe_obp_construct_config_ptr_f();
    if (status!=MOD_SOM_STATUS_OK){
      printf("EFE OBP not initialized\n");
      return status;
    }

    // ALB Allocate memory for the consumer_ptr,
    // ALB contains the consumer stream data_ptr and stream_data length.
    // ALB This pointer is also used to store the data on the SD card
      status |= mod_som_efe_obp_construct_consumer_ptr_f();
    if (status!=MOD_SOM_STATUS_OK){
      printf("EFE OBP not initialized\n");
      return status;
    }

    // ALB Allocate memory for the consumer_ptr,
    // ALB contains the consumer stream data_ptr and stream_data length.
    // ALB This pointer is also used to store the data on the SD card
      status |= mod_som_efe_obp_construct_fill_segment_ptr_f();
      status |= mod_som_efe_obp_construct_cpt_spectra_ptr_f();
      status |= mod_som_efe_obp_construct_cpt_dissrate_ptr_f();
    if (status!=MOD_SOM_STATUS_OK){
      printf("EFE OBP not initialized\n");
      return status;
    }

    local_sbe41_ptr=mod_som_sbe41_get_runtime_ptr_f();


    mod_som_efe_obp_ptr->sample_count                = 0;
    mod_som_efe_obp_ptr->sampling_flag               = 0;
    mod_som_efe_obp_ptr->data_ready_flag             = 0;
    mod_som_efe_obp_ptr->start_computation_timestamp = 0;
    mod_som_efe_obp_ptr->stop_computation_timestamp  = 0;
    mod_som_efe_obp_ptr->mode                        = 0;                       //ALB 0:stream, 1:store, 2: Aggregator, 3: apf
    mod_som_efe_obp_ptr->format                      = 0;                       //ALB 0:segment, 1:FFT, 2: rates, 3: segment+FFT+rates

    mod_som_efe_obp_ptr->initialized_flag            = true;

    printf("EFE OBP initialized\n");



    return mod_som_efe_obp_encode_status_f(MOD_SOM_STATUS_OK);

}

/*******************************************************************************
 * @brief
 *   a function to output hello
 *
 * @return
 *   MOD_SOM_STATUS_OK if function execute nicely
 ******************************************************************************/
mod_som_status_t mod_som_efe_obp_say_hello_world_f(){
    mod_som_io_print_f("[foo bar]: hello world\r\n");
    return mod_som_efe_obp_encode_status_f(MOD_SOM_STATUS_OK);
}

/*******************************************************************************
 * @brief
 *   construct settings_ptr
 *
 * @return
 *   MOD_SOM_STATUS_OK if initialization goes well
 *   or otherwise
 ******************************************************************************/
mod_som_status_t mod_som_efe_obp_allocate_settings_ptr_f(){

  RTOS_ERR  err;

  //ALB alloc memory for setup pointer
  //set up default configuration
  mod_som_efe_obp_ptr->settings_ptr =
      (mod_som_efe_obp_settings_ptr_t)Mem_SegAlloc(
          "MOD SOM EFE OBP setup.",DEF_NULL,
          sizeof(mod_som_efe_obp_settings_t),
          &err);

  // Check error code
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);
  if(mod_som_efe_obp_ptr->settings_ptr==NULL)
  {
    mod_som_efe_obp_ptr = DEF_NULL;
    return MOD_SOM_EFE_OBP_CANNOT_ALLOCATE_SETUP;
  }

  return mod_som_efe_obp_encode_status_f(MOD_SOM_STATUS_OK);
}

/*******************************************************************************
 * @brief
 *   Initialize setup struct ptr
 *   uint32_t size;
 *   char header[MOD_SOM_EFE_OBP_SETTINGS_STR_lENGTH];
 *   uint32_t nfft;
 *   uint32_t record_format;
 *   uint32_t telemetry_format;
 *   uint32_t initialize_flag;
 *
 * @param config_ptr
 *   configuration pointer
 ******************************************************************************/
mod_som_status_t mod_som_efe_obp_default_settings_f(
                                    mod_som_efe_obp_settings_ptr_t settings_ptr)
{
  // TODO It will always return MOD_SOM_STATUS_OK.
  // TODO I do not know how to check if everything is fine here.

  settings_ptr->nfft               = MOD_SOM_EFE_OBP_DEFAULT_NFFT;
  settings_ptr->record_format      = MOD_SOM_EFE_RECORD_DEFAULT_FORMAT;
  settings_ptr->telemetry_format   = MOD_SOM_EFE_TELEMETRY_DEFAULT_FORMAT;
  settings_ptr->degrees_of_freedom = MOD_SOM_EFE_OBP_CPT_SPECTRA_DEGREE_OF_FREEDOM;
  settings_ptr->channels_id[0]     = 0 ; //select channel t1
  settings_ptr->channels_id[1]     = 2 ; //select channel s1
  settings_ptr->channels_id[2]     = 6 ; //select channel a3

  strncpy(settings_ptr->header,
          MOD_SOM_EFE_OBP_HEADER,MOD_SOM_EFE_OBP_SETTINGS_STR_lENGTH);
  settings_ptr->initialize_flag=true;
  settings_ptr->size=sizeof(*settings_ptr);


  // get efe_settings: Nb of channels, record length, ...
  mod_som_efe_obp_ptr->efe_settings_ptr=mod_som_efe_get_settings_ptr_f();

  return mod_som_efe_encode_status_f(MOD_SOM_STATUS_OK);
}

/*******************************************************************************
 * @brief
 *   get the setup struct ptr
 *
 * @param config_ptr
 *   configuration pointer
 ******************************************************************************/
mod_som_efe_obp_settings_t mod_som_efe_obp_get_settings_f(){
  return *mod_som_efe_obp_ptr->settings_ptr;
}

/*******************************************************************************
 * @brief
 *   get the efe obp runtime ptr
 *
 * @param
 *
 ******************************************************************************/
mod_som_efe_obp_ptr_t mod_som_efe_obp_get_runtime_ptr_f(){
  return mod_som_efe_obp_ptr;
}


/*******************************************************************************
 * @brief
 *   construct config_ptr
 *
 * @return
 *   MOD_SOM_STATUS_OK if initialization goes well
 *   or otherwise
 ******************************************************************************/
mod_som_status_t mod_som_efe_obp_construct_config_ptr_f(){

  RTOS_ERR  err;
  //ALB Start allocating  memory for config pointer
  mod_som_efe_obp_ptr->config_ptr =
      (mod_som_efe_obp_config_ptr_t)Mem_SegAlloc(
          "MOD SOM EFE OBP config.",DEF_NULL,
          sizeof(mod_som_efe_obp_config_t),
          &err);
  // Check error code
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);
  if(mod_som_efe_obp_ptr->config_ptr==NULL)
  {
    mod_som_efe_obp_ptr = DEF_NULL;
    return MOD_SOM_EFE_OBP_CANNOT_OPEN_CONFIG;
  }

  mod_som_efe_obp_config_ptr_t config_ptr = mod_som_efe_obp_ptr->config_ptr;
  // TODO It will always return MOD_SOM_STATUS_OK.
  // TODO I do not know how to check if everything is fine here.

  config_ptr->initialized_flag = false;
  config_ptr->header_length    = MOD_SOM_EFE_OBP_SYNC_TAG_LENGTH      +
                                 MOD_SOM_EFE_OBP_HEADER_TAG_LENGTH    +
                                 MOD_SOM_EFE_OBP_HEXTIMESTAMP_LENGTH    +
                                 MOD_SOM_EFE_OBP_PAYLOAD_LENGTH         +
                                 MOD_SOM_EFE_OBP_LENGTH_HEADER_CHECKSUM;


  config_ptr->initialized_flag = true;
  return mod_som_efe_obp_encode_status_f(MOD_SOM_STATUS_OK);
}

/*******************************************************************************
 * @brief
 *   construct consumer structure
 *
 *   The consumer will creates 3 type of records: segment, spectra, rates.
 *   Each segments will follow the rules of mod som record:
 *   header + header checksum + payload + payload checksum
 *   header:  1  char sync ($)               +
 *            4  char tag (SEGM, SPEC, RATE) +
 *            16 char timestamps             +
 *            8  char payload size.
 *   header checksum: 1 char sync (*) +
 *                    2 char checksum (Hex of xor header)
 *   payload: data
 *   payload checksum : 1 char sync (*)                      +
 *                      2 char checksum (Hex of xor payload) +
 *                      2 char \r\n.
 *
 *
 * @return
 *   MOD_SOM_STATUS_OK if initialization goes well
 *   or otherwise
 ******************************************************************************/
mod_som_status_t mod_som_efe_obp_construct_consumer_ptr_f(){

  RTOS_ERR  err;


  // allocate memory for streaming_ptr
  mod_som_efe_obp_ptr->consumer_ptr =
      (mod_som_efe_obp_data_consumer_ptr_t)Mem_SegAlloc(
      "MOD SOM EFE OBP consumer ptr",DEF_NULL,
      sizeof(mod_som_efe_obp_data_consumer_t),
      &err);
  // Check error code
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);
  if(RTOS_ERR_CODE_GET(err) != RTOS_ERR_NONE)
    return (mod_som_efe_obp_ptr->status =
        mod_som_efe_encode_status_f(
            MOD_SOM_EFE_OBP_STATUS_FAIL_TO_ALLOCATE_MEMORY));

  if(mod_som_efe_obp_ptr->consumer_ptr==DEF_NULL)
  {
    mod_som_efe_obp_ptr = DEF_NULL;
    return (mod_som_efe_obp_ptr->status =
        mod_som_efe_obp_encode_status_f(
            MOD_SOM_EFE_OBP_STATUS_FAIL_TO_ALLOCATE_MEMORY));
  }

  //ALB allocating memory for the biggest record use in this consumer
  //ALB That would be for printing segment (timestamps + segment)
  //ALB I am going to print only one channel.
  mod_som_efe_obp_ptr->consumer_ptr->max_payload_length =
     sizeof(uint64_t)+
     (mod_som_efe_obp_ptr->settings_ptr->nfft*sizeof(float))*
      MOD_SOM_EFE_OBP_CONSUMER_NB_SEGMENT_PER_RECORD;

  mod_som_efe_obp_ptr->consumer_ptr->max_record_length=
      mod_som_efe_obp_ptr->config_ptr->header_length +
      mod_som_efe_obp_ptr->consumer_ptr->max_payload_length+
      MOD_SOM_EFE_OBP_CONSUMER_PAYLOAD_CHECKSUM_LENGTH;

  mod_som_efe_obp_ptr->consumer_ptr->length_header=
                                           MOD_SOM_EFE_OBP_SYNC_TAG_LENGTH+
                                           MOD_SOM_EFE_OBP_HEADER_TAG_LENGTH+
                                           MOD_SOM_EFE_OBP_HEXTIMESTAMP_LENGTH +
                                           MOD_SOM_EFE_OBP_SETTINGS_STR_lENGTH+
                                           MOD_SOM_EFE_OBP_LENGTH_HEADER_CHECKSUM;


  //ALB initialize counters.
  mod_som_efe_obp_ptr->consumer_ptr->mode         = spectra;
  mod_som_efe_obp_ptr->consumer_ptr->channel      = shear;
  mod_som_efe_obp_ptr->consumer_ptr->segment_cnt  = 0;
  mod_som_efe_obp_ptr->consumer_ptr->spectrum_cnt = 0;
  mod_som_efe_obp_ptr->consumer_ptr->rates_cnt    = 0;
  mod_som_efe_obp_ptr->consumer_ptr->started_flg  = false;

  //place holder allocation. It should actually point to the data file.

  mod_som_efe_obp_ptr->consumer_ptr->record_ptr =
      (uint8_t *)Mem_SegAlloc(
          "MOD SOM EFE OBP rec.",DEF_NULL,
          mod_som_efe_obp_ptr->consumer_ptr->max_record_length,
          &err);

  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);


  return mod_som_efe_encode_status_f(MOD_SOM_STATUS_OK);
}


/*******************************************************************************
 * @brief
 *   construct fill segment producer structure
 *
 * @return
 *   MOD_SOM_STATUS_OK if initialization goes well
 *   or otherwise
 ******************************************************************************/
mod_som_status_t mod_som_efe_obp_construct_fill_segment_ptr_f(){

  RTOS_ERR  err;


  // allocate memory for fill_segment_ptr
  mod_som_efe_obp_ptr->fill_segment_ptr = (mod_som_efe_obp_data_fill_segment_ptr_t)Mem_SegAlloc(
      "MOD SOM EFE OBP fill_segment ptr",DEF_NULL,
      sizeof(mod_som_efe_obp_data_fill_segment_t),
      &err);
  // Check error code
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);
  if(RTOS_ERR_CODE_GET(err) != RTOS_ERR_NONE)
    return (mod_som_efe_obp_ptr->status = mod_som_efe_encode_status_f(MOD_SOM_EFE_OBP_STATUS_FAIL_TO_ALLOCATE_MEMORY));
  if(mod_som_efe_obp_ptr->fill_segment_ptr==DEF_NULL)
  {
    mod_som_efe_obp_ptr = DEF_NULL;
    return (mod_som_efe_obp_ptr->status = mod_som_efe_obp_encode_status_f(MOD_SOM_EFE_OBP_STATUS_FAIL_TO_ALLOCATE_MEMORY));
  }

  // ALB Allocate memory for the spectra

  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);
  mod_som_efe_obp_ptr->fill_segment_ptr->seg_temp_volt_ptr = (float *)Mem_SegAlloc(
        "MOD SOM EFE OBP temp volt ptr",DEF_NULL,
        MOD_SOM_EFE_OBP_FILL_SEGMENT_NB_SEGMENT_PER_RECORD*
        sizeof(float)*mod_som_efe_obp_ptr->settings_ptr->nfft,
        &err);
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);
  mod_som_efe_obp_ptr->fill_segment_ptr->seg_shear_volt_ptr = (float *)Mem_SegAlloc(
        "MOD SOM EFE OBP shear volt ptr",DEF_NULL,
        MOD_SOM_EFE_OBP_FILL_SEGMENT_NB_SEGMENT_PER_RECORD*
        sizeof(float)*mod_som_efe_obp_ptr->settings_ptr->nfft,
        &err);
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);
  mod_som_efe_obp_ptr->fill_segment_ptr->seg_accel_volt_ptr = (float *)Mem_SegAlloc(
        "MOD SOM EFE OBP accel volt ptr",DEF_NULL,
        MOD_SOM_EFE_OBP_FILL_SEGMENT_NB_SEGMENT_PER_RECORD*
        sizeof(float)*mod_som_efe_obp_ptr->settings_ptr->nfft,
        &err);
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);


  //ALB initialize field ctd data
  mod_som_efe_obp_ptr->fill_segment_ptr->avg_ctd_pressure=0;
  mod_som_efe_obp_ptr->fill_segment_ptr->avg_ctd_temperature=0;
  mod_som_efe_obp_ptr->fill_segment_ptr->avg_ctd_salinity=0;
  mod_som_efe_obp_ptr->fill_segment_ptr->avg_ctd_fallrate=0;

  //ALB initialize all parameters. They should be reset right before
  //ALB fill_segment task is starts running.
  mod_som_efe_obp_ptr->fill_segment_ptr->timestamp_segment=0;
  mod_som_efe_obp_ptr->fill_segment_ptr->segment_skipped=0;
  mod_som_efe_obp_ptr->fill_segment_ptr->ctd_element_cnt=0;
  mod_som_efe_obp_ptr->fill_segment_ptr->efe_element_cnt=0;
  mod_som_efe_obp_ptr->fill_segment_ptr->efe_element_skipped=0;
  mod_som_efe_obp_ptr->fill_segment_ptr->segment_cnt=0;
  mod_som_efe_obp_ptr->fill_segment_ptr->half_segment_cnt=0;
  mod_som_efe_obp_ptr->fill_segment_ptr->started_flg=false;
  mod_som_efe_obp_ptr->fill_segment_ptr->segment_skipped=0;


  return mod_som_efe_obp_encode_status_f(MOD_SOM_STATUS_OK);
}



/*******************************************************************************
 * @brief
 *   construct fill spectra producer structure
 *
 * @return
 *   MOD_SOM_STATUS_OK if initialization goes well
 *   or otherwise
 ******************************************************************************/
mod_som_status_t mod_som_efe_obp_construct_cpt_spectra_ptr_f(){

  RTOS_ERR  err;


  // allocate memory for cpt_spectra_ptr
  mod_som_efe_obp_ptr->cpt_spectra_ptr = (mod_som_efe_obp_data_cpt_spectra_ptr_t)Mem_SegAlloc(
      "MOD SOM EFE OBP cpt_spectra ptr",DEF_NULL,
      sizeof(mod_som_efe_obp_data_cpt_spectra_t),
      &err);
  // Check error code
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);
  if(RTOS_ERR_CODE_GET(err) != RTOS_ERR_NONE)
    return (mod_som_efe_obp_ptr->status = mod_som_efe_encode_status_f(MOD_SOM_EFE_OBP_STATUS_FAIL_TO_ALLOCATE_MEMORY));
  if(mod_som_efe_obp_ptr->fill_segment_ptr==DEF_NULL)
  {
    mod_som_efe_obp_ptr = DEF_NULL;
    return (mod_som_efe_obp_ptr->status = mod_som_efe_obp_encode_status_f(MOD_SOM_EFE_OBP_STATUS_FAIL_TO_ALLOCATE_MEMORY));
  }


  // ALB Allocate memory for the spectra
  mod_som_efe_obp_ptr->cpt_spectra_ptr->spec_temp_ptr = (float *)Mem_SegAlloc(
        "MOD SOM EFE OBP temp spectrum ptr",DEF_NULL,
        MOD_SOM_EFE_OBP_CPT_SPECTRA_NB_SPECTRA_PER_RECORD*
        sizeof(float)*mod_som_efe_obp_ptr->settings_ptr->nfft/2,
        &err);
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);
  mod_som_efe_obp_ptr->cpt_spectra_ptr->spec_shear_ptr = (float *)Mem_SegAlloc(
        "MOD SOM EFE OBP shear spectrum ptr",DEF_NULL,
        MOD_SOM_EFE_OBP_CPT_SPECTRA_NB_SPECTRA_PER_RECORD*
        sizeof(float)*mod_som_efe_obp_ptr->settings_ptr->nfft/2,
        &err);
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);
  mod_som_efe_obp_ptr->cpt_spectra_ptr->spec_accel_ptr = (float *)Mem_SegAlloc(
        "MOD SOM EFE OBP accel spectrum ptr",DEF_NULL,
        MOD_SOM_EFE_OBP_CPT_SPECTRA_NB_SPECTRA_PER_RECORD*
        sizeof(float)*mod_som_efe_obp_ptr->settings_ptr->nfft/2,
        &err);
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);


  //ALB initialize field ctd data
  mod_som_efe_obp_ptr->cpt_spectra_ptr->avg_ctd_pressure    =0;
  mod_som_efe_obp_ptr->cpt_spectra_ptr->avg_ctd_temperature =0;
  mod_som_efe_obp_ptr->cpt_spectra_ptr->avg_ctd_salinity    =0;
  mod_som_efe_obp_ptr->cpt_spectra_ptr->avg_ctd_fallrate    =0;

  //ALB initialize all parameters. They should be reset right before
  //ALB cpt_spectra task is starts running.

  mod_som_efe_obp_ptr->cpt_spectra_ptr->avg_timestamp=0;
  mod_som_efe_obp_ptr->cpt_spectra_ptr->dof=0;
  mod_som_efe_obp_ptr->cpt_spectra_ptr->started_flg=false;
  mod_som_efe_obp_ptr->cpt_spectra_ptr->spectra_skipped=0;
  mod_som_efe_obp_ptr->cpt_spectra_ptr->spectrum_cnt=0;
  mod_som_efe_obp_ptr->cpt_spectra_ptr->volt_read_index=0;

  return mod_som_efe_obp_encode_status_f(MOD_SOM_STATUS_OK);
}


/*******************************************************************************
 * @brief
 *   construct fill segment producer structure
 *
 * @return
 *   MOD_SOM_STATUS_OK if initialization goes well
 *   or otherwise
 ******************************************************************************/
mod_som_status_t mod_som_efe_obp_construct_cpt_dissrate_ptr_f(){

  RTOS_ERR  err;


  // allocate memory for streaming_ptr
  mod_som_efe_obp_ptr->cpt_dissrate_ptr = (mod_som_efe_obp_data_cpt_dissrate_ptr_t)Mem_SegAlloc(
      "MOD SOM EFE OBP cpt_dissrate ptr",DEF_NULL,
      sizeof(mod_som_efe_obp_data_cpt_dissrate_t),
      &err);
  // Check error code
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);
  if(RTOS_ERR_CODE_GET(err) != RTOS_ERR_NONE)
    return (mod_som_efe_obp_ptr->status = mod_som_efe_encode_status_f(MOD_SOM_EFE_OBP_STATUS_FAIL_TO_ALLOCATE_MEMORY));
  if(mod_som_efe_obp_ptr->fill_segment_ptr==DEF_NULL)
  {
    mod_som_efe_obp_ptr = DEF_NULL;
    return (mod_som_efe_obp_ptr->status = mod_som_efe_obp_encode_status_f(MOD_SOM_EFE_OBP_STATUS_FAIL_TO_ALLOCATE_MEMORY));
  }


  // ALB Allocate memory for the dissrate avg temp spec
  mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_spec_temp_ptr= (float *)Mem_SegAlloc(
      "MOD SOM EFE OBP dissrate avg temp spec ptr",DEF_NULL,
      MOD_SOM_EFE_OBP_CPT_DISSRATE_NB_AVG_SPECTRA_PER_RECORD*
      sizeof(float)*mod_som_efe_obp_ptr->settings_ptr->nfft/2,
      &err);
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);

  // ALB Allocate memory for the dissrate avg shear spec
  mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_spec_shear_ptr= (float *)Mem_SegAlloc(
      "MOD SOM EFE OBP dissrate avg shear spec ptr",DEF_NULL,
      MOD_SOM_EFE_OBP_CPT_DISSRATE_NB_AVG_SPECTRA_PER_RECORD*
      sizeof(float)*mod_som_efe_obp_ptr->settings_ptr->nfft/2,
      &err);
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);

  // ALB Allocate memory for the dissrate avg accel spec
  mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_spec_accel_ptr= (float *)Mem_SegAlloc(
      "MOD SOM EFE OBP dissrate avg accel spec ptr",DEF_NULL,
      MOD_SOM_EFE_OBP_CPT_DISSRATE_NB_AVG_SPECTRA_PER_RECORD*
      sizeof(float)*mod_som_efe_obp_ptr->settings_ptr->nfft/2,
      &err);
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);



 mod_som_efe_obp_ptr->cpt_dissrate_ptr->chi = (float *)Mem_SegAlloc(
        "MOD SOM EFE OBP dissrate chi ptr",DEF_NULL,
        MOD_SOM_EFE_OBP_CPT_DISSRATE_NB_RATES_PER_RECORD*sizeof(float),
        &err);
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);

  mod_som_efe_obp_ptr->cpt_dissrate_ptr->epsilon = (float *)Mem_SegAlloc(
        "MOD SOM EFE OBP dissrate epsilon ptr",DEF_NULL,
        MOD_SOM_EFE_OBP_CPT_DISSRATE_NB_RATES_PER_RECORD*sizeof(float),
        &err);
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);

  mod_som_efe_obp_ptr->cpt_dissrate_ptr->nu = (float *)Mem_SegAlloc(
        "MOD SOM EFE OBP dissrate nu ptr",DEF_NULL,
        MOD_SOM_EFE_OBP_CPT_DISSRATE_NB_RATES_PER_RECORD*sizeof(float),
        &err);
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);

  mod_som_efe_obp_ptr->cpt_dissrate_ptr->kappa = (float *)Mem_SegAlloc(
        "MOD SOM EFE OBP dissrate kappa ptr",DEF_NULL,
        MOD_SOM_EFE_OBP_CPT_DISSRATE_NB_RATES_PER_RECORD*sizeof(float),
        &err);
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);


  //ALB initialize field ctd data
  mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_ctd_pressure    =0;
  mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_ctd_temperature =0;
  mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_ctd_salinity    =0;
  mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_ctd_fallrate    =0;

  //ALB initialize all parameters. They should be reset right before
  //ALB cpt_spectra task is starts running.
  mod_som_efe_obp_ptr->cpt_dissrate_ptr->dof_flag=false;
  mod_som_efe_obp_ptr->cpt_dissrate_ptr->dissrates_cnt=0;
  mod_som_efe_obp_ptr->cpt_dissrate_ptr->spectrum_cnt=0;
  mod_som_efe_obp_ptr->cpt_dissrate_ptr->dissrate_skipped=0;
  mod_som_efe_obp_ptr->cpt_dissrate_ptr->started_flg=false;


  return mod_som_efe_obp_encode_status_f(MOD_SOM_STATUS_OK);
}


/*******************************************************************************
 * @brief
 *   stop fill segment task
 *
 * @return
 *   MOD_SOM_STATUS_OK if initialization goes well
 *   or otherwise
 ******************************************************************************/
mod_som_status_t mod_som_efe_obp_stop_fill_segment_task_f(){


  RTOS_ERR err;
  OSTaskDel(&efe_obp_fill_segment_task_tcb,
             &err);

//  mod_som_efe_obp_ptr->started_flag=false;


  if(RTOS_ERR_CODE_GET(err) != RTOS_ERR_NONE)
    return (mod_som_efe_obp_ptr->status = mod_som_efe_encode_status_f(MOD_SOM_EFE_OPB_STATUS_FAIL_TO_START_CONSUMER_TASK));

  return mod_som_efe_encode_status_f(MOD_SOM_STATUS_OK);
}



/*******************************************************************************
 * @brief
 *   stop compute spectra task
 *
 * @return
 *   MOD_SOM_STATUS_OK if initialization goes well
 *   or otherwise
 ******************************************************************************/
mod_som_status_t mod_som_efe_obp_stop_cpt_spectra_task_f(){


  RTOS_ERR err;
  OSTaskDel(&efe_obp_cpt_spectra_task_tcb,
             &err);

//  mod_som_efe_obp_ptr->started_flag=false;


  if(RTOS_ERR_CODE_GET(err) != RTOS_ERR_NONE)
    return (mod_som_efe_obp_ptr->status = mod_som_efe_encode_status_f(MOD_SOM_EFE_OPB_STATUS_FAIL_TO_START_CONSUMER_TASK));

  return mod_som_efe_encode_status_f(MOD_SOM_STATUS_OK);
}


/*******************************************************************************
 * @brief
 *   stop cpt dissrate task
 *
 * @return
 *   MOD_SOM_STATUS_OK if initialization goes well
 *   or otherwise
 ******************************************************************************/
mod_som_status_t mod_som_efe_obp_stop_cpt_dissrate_task_f(){


  RTOS_ERR err;
  OSTaskDel(&efe_obp_cpt_dissrate_task_tcb,
             &err);

//  mod_som_efe_obp_ptr->started_flag=false;


  if(RTOS_ERR_CODE_GET(err) != RTOS_ERR_NONE)
    return (mod_som_efe_obp_ptr->status = mod_som_efe_encode_status_f(MOD_SOM_EFE_OPB_STATUS_FAIL_TO_START_CONSUMER_TASK));

  return mod_som_efe_encode_status_f(MOD_SOM_STATUS_OK);
}



/*******************************************************************************
 * @brief
 *   start fill segment task
 *
 * @return
 *   MOD_SOM_STATUS_OK if initialization goes well
 *   or otherwise
 ******************************************************************************/
mod_som_status_t mod_som_efe_obp_start_fill_segment_task_f(){


  RTOS_ERR err;
  // Consumer Task 2
  //ALB initialize all parameters. They should be reset right before
  //ALB fill_segment task is starts running.
  mod_som_efe_obp_ptr->fill_segment_ptr->segment_skipped=0;
  mod_som_efe_obp_ptr->fill_segment_ptr->ctd_element_cnt=0;
  mod_som_efe_obp_ptr->fill_segment_ptr->efe_element_cnt=0;
  mod_som_efe_obp_ptr->fill_segment_ptr->efe_element_skipped=0;
  mod_som_efe_obp_ptr->fill_segment_ptr->segment_cnt=0;
  mod_som_efe_obp_ptr->fill_segment_ptr->half_segment_cnt=0;
  mod_som_efe_obp_ptr->fill_segment_ptr->segment_skipped=0;
  mod_som_efe_obp_ptr->fill_segment_ptr->started_flg=true;

  //ALB initialize field ctd data
  mod_som_efe_obp_ptr->fill_segment_ptr->avg_ctd_pressure=0;
  mod_som_efe_obp_ptr->fill_segment_ptr->avg_ctd_temperature=0;
  mod_som_efe_obp_ptr->fill_segment_ptr->avg_ctd_salinity=0;
  mod_som_efe_obp_ptr->fill_segment_ptr->avg_ctd_fallrate=0;

   OSTaskCreate(&efe_obp_fill_segment_task_tcb,
                        "efe obp fill segment task",
                        mod_som_efe_obp_fill_segment_task_f,
                        DEF_NULL,
                        MOD_SOM_EFE_OBP_FILL_SEGMENT_TASK_PRIO,
            &efe_obp_fill_segment_task_stk[0],
            (MOD_SOM_EFE_OBP_FILL_SEGMENT_TASK_STK_SIZE / 10u),
             MOD_SOM_EFE_OBP_FILL_SEGMENT_TASK_STK_SIZE,
            0u,
            0u,
            DEF_NULL,
            (OS_OPT_TASK_STK_CLR),
            &err);


  // Check error code
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);
  if(RTOS_ERR_CODE_GET(err) != RTOS_ERR_NONE)
    return (mod_som_efe_obp_ptr->status = mod_som_efe_encode_status_f(MOD_SOM_EFE_OPB_STATUS_FAIL_TO_START_CONSUMER_TASK));
  return mod_som_efe_encode_status_f(MOD_SOM_STATUS_OK);
}



/*******************************************************************************
 * @brief
 *   create producer task
 *
 * @return
 *   MOD_SOM_STATUS_OK if initialization goes well
 *   or otherwise
 ******************************************************************************/
mod_som_status_t mod_som_efe_obp_start_cpt_spectra_task_f(){


  RTOS_ERR err;
  // Consumer Task 2
  mod_som_efe_obp_ptr->cpt_spectra_ptr->avg_timestamp=0;
   mod_som_efe_obp_ptr->cpt_spectra_ptr->dof=0;
   mod_som_efe_obp_ptr->cpt_spectra_ptr->spectra_skipped=0;
   mod_som_efe_obp_ptr->cpt_spectra_ptr->spectrum_cnt=0;
   mod_som_efe_obp_ptr->cpt_spectra_ptr->volt_read_index=0;
   mod_som_efe_obp_ptr->cpt_spectra_ptr->started_flg=true;

   //ALB initialize field ctd data
   mod_som_efe_obp_ptr->cpt_spectra_ptr->avg_ctd_pressure=0;
   mod_som_efe_obp_ptr->cpt_spectra_ptr->avg_ctd_temperature=0;
   mod_som_efe_obp_ptr->cpt_spectra_ptr->avg_ctd_salinity=0;
   mod_som_efe_obp_ptr->cpt_spectra_ptr->avg_ctd_fallrate=0;


   OSTaskCreate(&efe_obp_cpt_spectra_task_tcb,
                        "efe obp spectra task",
                        mod_som_efe_obp_cpt_spectra_task_f,
                        DEF_NULL,
                        MOD_SOM_EFE_OBP_CPT_SPECTRA_TASK_PRIO,
            &efe_obp_cpt_spectra_task_stk[0],
            (MOD_SOM_EFE_OBP_CPT_SPECTRA_TASK_STK_SIZE / 10u),
            MOD_SOM_EFE_OBP_CPT_SPECTRA_TASK_STK_SIZE,
            0u,
            0u,
            DEF_NULL,
            (OS_OPT_TASK_STK_CLR),
            &err);


  // Check error code
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);
  if(RTOS_ERR_CODE_GET(err) != RTOS_ERR_NONE)
    return (mod_som_efe_obp_ptr->status = mod_som_efe_encode_status_f(MOD_SOM_EFE_OPB_STATUS_FAIL_TO_START_CONSUMER_TASK));
  return mod_som_efe_encode_status_f(MOD_SOM_STATUS_OK);
}




/*******************************************************************************
 * @brief
 *   create producer task
 *
 * @return
 *   MOD_SOM_STATUS_OK if initialization goes well
 *   or otherwise
 ******************************************************************************/
mod_som_status_t mod_som_efe_obp_start_cpt_dissrate_task_f(){


  RTOS_ERR err;
  // Consumer Task 2
  mod_som_efe_obp_ptr->cpt_dissrate_ptr->dof_flag=false;
  mod_som_efe_obp_ptr->cpt_dissrate_ptr->dissrates_cnt    = 0;
  mod_som_efe_obp_ptr->cpt_dissrate_ptr->spectrum_cnt     = 0;
  mod_som_efe_obp_ptr->cpt_dissrate_ptr->dissrate_skipped = 0;
  mod_som_efe_obp_ptr->cpt_dissrate_ptr->started_flg      = true;


  //ALB initialize field ctd data
  mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_ctd_pressure=0;
  mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_ctd_temperature=0;
  mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_ctd_salinity=0;
  mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_ctd_fallrate=0;


   OSTaskCreate(&efe_obp_cpt_dissrate_task_tcb,
                        "efe obp cpt_dissrate task",
                        mod_som_efe_obp_cpt_dissrate_task_f,
                        DEF_NULL,
                        MOD_SOM_EFE_OBP_CPT_DISSRATE_TASK_PRIO,
            &efe_obp_cpt_dissrate_task_stk[0],
            (MOD_SOM_EFE_OBP_CPT_DISSRATE_TASK_STK_SIZE / 10u),
            MOD_SOM_EFE_OBP_CPT_DISSRATE_TASK_STK_SIZE,
            0u,
            0u,
            DEF_NULL,
            (OS_OPT_TASK_STK_CLR),
            &err);


  // Check error code
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);
  if(RTOS_ERR_CODE_GET(err) != RTOS_ERR_NONE)
    return (mod_som_efe_obp_ptr->status = mod_som_efe_encode_status_f(MOD_SOM_EFE_OPB_STATUS_FAIL_TO_START_CONSUMER_TASK));
  return mod_som_efe_encode_status_f(MOD_SOM_STATUS_OK);
}


/*******************************************************************************
 * @brief
 *   fill_segment task function
 *
 *   gather, convert cnt to volt and fill the segment float array
 *
 * @return
 *   MOD_SOM_STATUS_OK if initialization goes well
 *   or otherwise
 ******************************************************************************/
void mod_som_efe_obp_fill_segment_task_f(void  *p_arg){
  RTOS_ERR  err;

  (void)p_arg; // Deliberately unused argument

  float    * local_efeobp_efe_temp_ptr;
  float    * local_efeobp_efe_shear_ptr;
  float    * local_efeobp_efe_accel_ptr;

  uint8_t efe_element_length=MOD_SOM_EFE_TIMESTAMP_LENGTH+                     \
                    mod_som_efe_obp_ptr->efe_settings_ptr->number_of_channels* \
                            AD7124_SAMPLE_LENGTH;
  uint8_t local_efe_sample[mod_som_efe_obp_ptr->efe_settings_ptr->number_of_channels* \
                           AD7124_SAMPLE_LENGTH];

  int elmnts_avail=0, reset_segment_cnt=0;
  int data_elmnts_offset=0;
  int fill_segment_offset=0;

  uint32_t cnsmr_indx=0;

  int padding = 5; // the padding should be big enough to include the time variance.

  //ALB GET the efe run time structure
  mod_som_efe_ptr_t local_mod_som_efe_ptr = mod_som_efe_get_runtime_ptr_f();
  uint8_t * curr_data_ptr   = local_mod_som_efe_ptr->rec_buff->efe_elements_buffer;

  while (DEF_ON) {



      /************************************************************************/
      //ALB phase 1
      //ALB append the new block of data to the volt segment
      //ALB parse timestamp and efe data
      //ALB convert EFE samples into volts
      //ALB fill the volt-producer buffers
      if (mod_som_efe_obp_ptr->fill_segment_ptr->started_flg){
          elmnts_avail = local_mod_som_efe_ptr->sample_count -
              mod_som_efe_obp_ptr->fill_segment_ptr->efe_element_cnt;  //calculate number of elements available have been produced

          //ALB User stopped efe. I need to reset the obp producers count
          if(elmnts_avail<0){
              mod_som_efe_obp_ptr->fill_segment_ptr->efe_element_cnt = 0;
              mod_som_efe_obp_ptr->fill_segment_ptr->segment_cnt     = 0;
              mod_som_efe_obp_ptr->cpt_spectra_ptr->spectrum_cnt     = 0;
              mod_som_efe_obp_ptr->cpt_dissrate_ptr->dissrates_cnt   = 0;
          }
          // LOOP without delay until caught up to latest produced element
          while (elmnts_avail > 0)
            {
              // When have circular buffer overflow: have produced data bigger than consumer data: 1 circular buffer (n_elmnts)
              // calculate new consumer count to skip ahead to the tail of the circular buffer (with optional padding),
              // calculate the number of data we skipped, report number of elements skipped.
              // Reset the consumers cnt equal with producer data plus padding
              if (elmnts_avail>local_mod_som_efe_ptr->config_ptr->element_per_buffer){ // checking over flow. TODO check adding padding is correct.
                  // reset the consumer count less one buffer than producer count plus padding
                  //ALB I think I want to change this line from the cb example. The "-" only works if you overflowed once.
                  reset_segment_cnt = local_mod_som_efe_ptr->sample_count -
                      local_mod_som_efe_ptr->config_ptr->element_per_buffer +
                      padding;
                  // calculate the number of skipped elements
                  mod_som_efe_obp_ptr->fill_segment_ptr->efe_element_skipped = reset_segment_cnt -
                      mod_som_efe_obp_ptr->fill_segment_ptr->efe_element_cnt;

                  mod_som_io_print_f("\n efe obp fill seg task: CB overflow: sample count = %lu,"
                      "cnsmr_cnt = %lu,skipped %lu elements \r\n ", \
                      (uint32_t)local_mod_som_efe_ptr->sample_count, \
                      (uint32_t)mod_som_efe_obp_ptr->fill_segment_ptr->efe_element_cnt, \
                      (uint32_t)mod_som_efe_obp_ptr->fill_segment_ptr->efe_element_skipped);

                  mod_som_efe_obp_ptr->fill_segment_ptr->efe_element_cnt = reset_segment_cnt;
              }

              // calculate the offset for current pointer
              data_elmnts_offset = mod_som_efe_obp_ptr->fill_segment_ptr->efe_element_cnt %
                                   local_mod_som_efe_ptr->config_ptr->element_per_buffer;

              fill_segment_offset     = mod_som_efe_obp_ptr->fill_segment_ptr->efe_element_cnt %
                  (MOD_SOM_EFE_OBP_FILL_SEGMENT_NB_SEGMENT_PER_RECORD*
                  mod_som_efe_obp_ptr->settings_ptr->nfft);

              // update the current element pointer using the element map
              curr_data_ptr   =
                 (uint8_t*)
                 local_mod_som_efe_ptr->config_ptr->element_map[data_elmnts_offset];

              if((mod_som_efe_obp_ptr->fill_segment_ptr->efe_element_cnt%
                  mod_som_efe_obp_ptr->settings_ptr->nfft/2)==0){
                //ALB copy the timestamp of the mid-segment (nfft/2)
                //ALB (to bootstrap the segment in time and  save sapce )
                memcpy(&mod_som_efe_obp_ptr->fill_segment_ptr->timestamp_segment,
                       (uint64_t*) curr_data_ptr,
                       sizeof(uint64_t));
              }

              local_efeobp_efe_temp_ptr =
                  mod_som_efe_obp_ptr->fill_segment_ptr->seg_temp_volt_ptr +
                  fill_segment_offset;
              local_efeobp_efe_shear_ptr =
                  mod_som_efe_obp_ptr->fill_segment_ptr->seg_shear_volt_ptr +
                  fill_segment_offset;
              local_efeobp_efe_accel_ptr =
                  mod_som_efe_obp_ptr->fill_segment_ptr->seg_accel_volt_ptr +
                  fill_segment_offset;



              //ALB save each efe_sample inside the efe_block in a local_efe_sample;
              memcpy(local_efe_sample,
                     curr_data_ptr+MOD_SOM_EFE_TIMESTAMP_LENGTH,
                     efe_element_length);
              //ALB
              //local efe sample contains only the ADC samples.
              //convert the local efe sample from counts to volts
              //store the results directly in mod_som_efe_obp_ptr->producer_ptr->volt_ptr
              mod_som_efe_obp_cnt2volt_f(local_efe_sample,
                                         local_efeobp_efe_temp_ptr,
                                         local_efeobp_efe_shear_ptr,
                                         local_efeobp_efe_accel_ptr);


              mod_som_efe_obp_ptr->fill_segment_ptr->efe_element_cnt++;  // increment cnsmr count
              elmnts_avail = local_mod_som_efe_ptr->sample_count -
                      mod_som_efe_obp_ptr->fill_segment_ptr->efe_element_cnt; //elements available have been produced

              cnsmr_indx=local_sbe41_ptr->consumer_ptr->cnsmr_cnt % \
                          local_sbe41_ptr->consumer_ptr->max_element_per_record;

              if(local_sbe41_ptr->collect_data_flag){
                  mod_som_efe_obp_ptr->fill_segment_ptr->avg_ctd_pressure +=
                          local_sbe41_ptr->consumer_ptr->record_pressure[cnsmr_indx];
                  mod_som_efe_obp_ptr->fill_segment_ptr->avg_ctd_temperature +=
                          local_sbe41_ptr->consumer_ptr->record_temp[cnsmr_indx];
                  mod_som_efe_obp_ptr->fill_segment_ptr->avg_ctd_salinity +=
                          local_sbe41_ptr->consumer_ptr->record_salinity[cnsmr_indx];
                  mod_som_efe_obp_ptr->fill_segment_ptr->avg_ctd_pressure +=
                          local_sbe41_ptr->consumer_ptr->record_pressure[cnsmr_indx];
                  mod_som_efe_obp_ptr->fill_segment_ptr->avg_ctd_fallrate+=
                      local_sbe41_ptr->consumer_ptr->dPdt;
              }

              if((mod_som_efe_obp_ptr->fill_segment_ptr->efe_element_cnt %
                  (mod_som_efe_obp_ptr->settings_ptr->nfft/2)) ==0){
                  mod_som_efe_obp_ptr->fill_segment_ptr->half_segment_cnt++;
              }

              if((mod_som_efe_obp_ptr->fill_segment_ptr->efe_element_cnt %
                  (mod_som_efe_obp_ptr->settings_ptr->nfft)) ==0){
                  mod_som_efe_obp_ptr->fill_segment_ptr->segment_cnt++;

                  mod_som_efe_obp_ptr->fill_segment_ptr->avg_ctd_pressure/=
                      mod_som_efe_obp_ptr->settings_ptr->nfft;
                  mod_som_efe_obp_ptr->fill_segment_ptr->avg_ctd_pressure/=
                      mod_som_efe_obp_ptr->settings_ptr->nfft;
                  mod_som_efe_obp_ptr->fill_segment_ptr->avg_ctd_pressure/=
                      mod_som_efe_obp_ptr->settings_ptr->nfft;
                  mod_som_efe_obp_ptr->fill_segment_ptr->avg_ctd_fallrate/=
                      mod_som_efe_obp_ptr->settings_ptr->nfft;
              }
            }  // end of while (elemts_avail > 0)

          // ALB done with segment storing.
          mod_som_efe_obp_ptr->fill_segment_ptr->segment_skipped = 0;


     } //end while DEF_ON

      // Delay Start Task execution for
      OSTimeDly( MOD_SOM_EFE_OBP_FILL_SEGMENT_DELAY,             //   consumer delay is #define at the beginning OS Ticks
                 OS_OPT_TIME_DLY,          //   from now.
                 &err);
      //   Check error code.
      APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), ;);
//      printf("EFEOBP efe: %lu\r\n",(uint32_t) mod_som_efe_obp_ptr->fill_segment_ptr->efe_element_cnt);
//      printf("EFEOBP seg: %lu\r\n",(uint32_t) mod_som_efe_obp_ptr->fill_segment_ptr->segment_cnt);
  } // end of while (DEF_ON)

  PP_UNUSED_PARAM(p_arg);                                     // Prevent config warning.

}


/*******************************************************************************
 * @brief
 *   producer cpt spectra function
 *
 *   this will be a state machine:
 *   compute freq spectra and mean time of the segment(i.e., segment unique timestamp)
 *
 * @return
 *   MOD_SOM_STATUS_OK if initialization goes well
 *   or otherwise
 ******************************************************************************/
void mod_som_efe_obp_cpt_spectra_task_f(void  *p_arg){

  RTOS_ERR  err;
  uint64_t tick;

  (void)p_arg; // Deliberately unused argument


  int i;
  int segment_avail=0,reset_spectra_cnt=0;
  int segment_offset=0;
  int padding = 0; // the padding should be big enough to include the time variance.

  float * curr_temp_seg_ptr;
  float * curr_shear_seg_ptr;
  float * curr_accel_seg_ptr;

  while (DEF_ON) {



      /************************************************************************/
      //ALB phase 1
      //ALB check for available spectra
      //ALB compute the FFT.
      //ALB
      //ALB
      if (mod_som_efe_obp_ptr->cpt_spectra_ptr->started_flg &
          (mod_som_efe_obp_ptr->fill_segment_ptr->segment_cnt>=1)){
          //ALB half_segment_cnt-1 so I get the number of segment available with 50 % overlap;
          segment_avail = mod_som_efe_obp_ptr->fill_segment_ptr->half_segment_cnt-1 -
              mod_som_efe_obp_ptr->cpt_spectra_ptr->spectrum_cnt;  //calculate number of elements available have been produced
          // LOOP without delay until caught up to latest produced element
          while (segment_avail > 0)
            {
              // When have circular buffer overflow: have produced data bigger than consumer data: 1 circular buffer (n_elmnts)
              // calculate new consumer count to skip ahead to the tail of the circular buffer (with optional padding),
              // calculate the number of data we skipped, report number of elements skipped.
              // Reset the consumers cnt equal with producer data plus padding

              if (segment_avail>(2*MOD_SOM_EFE_OBP_FILL_SEGMENT_NB_SEGMENT_PER_RECORD)-1){ // checking over flow. TODO check adding padding is correct.
                  // reset the consumer count less one buffer than producer count plus padding
                  //ALB I think I want to change this line from the cb example. The "-" only works if you overflowed once.
                  reset_spectra_cnt = mod_som_efe_obp_ptr->fill_segment_ptr->half_segment_cnt -
                                    (2*MOD_SOM_EFE_OBP_FILL_SEGMENT_NB_SEGMENT_PER_RECORD) +
                                    padding;
                  // calculate the number of skipped elements
                  mod_som_efe_obp_ptr->fill_segment_ptr->segment_skipped = reset_spectra_cnt -
                      mod_som_efe_obp_ptr->cpt_spectra_ptr->spectrum_cnt;

                  mod_som_io_print_f("\n efe obp cpt spec: CB overflow: sample count = %lu,"
                      "cnsmr_cnt = %lu,skipped %lu elements \r\n ", \
                      (uint32_t)mod_som_efe_obp_ptr->fill_segment_ptr->segment_cnt, \
                      (uint32_t)mod_som_efe_obp_ptr->cpt_spectra_ptr->spectrum_cnt, \
                      (uint32_t)mod_som_efe_obp_ptr->fill_segment_ptr->segment_skipped);

                  mod_som_efe_obp_ptr->cpt_spectra_ptr->spectrum_cnt = reset_spectra_cnt;
              }

              // calculate the offset for current pointer
              segment_offset = (mod_som_efe_obp_ptr->cpt_spectra_ptr->spectrum_cnt*
                                    mod_som_efe_obp_ptr->settings_ptr->nfft/2) %
                                   (MOD_SOM_EFE_OBP_FILL_SEGMENT_NB_SEGMENT_PER_RECORD*
                                    mod_som_efe_obp_ptr->settings_ptr->nfft);

              // update the current temperature segment pointer
              curr_temp_seg_ptr   =
                 mod_som_efe_obp_ptr->fill_segment_ptr->seg_temp_volt_ptr+
                 segment_offset;

              curr_shear_seg_ptr   =
                  mod_som_efe_obp_ptr->fill_segment_ptr->seg_shear_volt_ptr+
                  segment_offset;

              curr_accel_seg_ptr   =
                  mod_som_efe_obp_ptr->fill_segment_ptr->seg_accel_volt_ptr+
                  segment_offset;

              mod_som_efe_obp_ptr->cpt_spectra_ptr->avg_timestamp=
                  mod_som_efe_obp_ptr->fill_segment_ptr->timestamp_segment;

              mod_som_efe_obp_ptr->cpt_spectra_ptr->avg_ctd_pressure=
                  mod_som_efe_obp_ptr->fill_segment_ptr->avg_ctd_pressure;

              mod_som_efe_obp_ptr->cpt_spectra_ptr->avg_ctd_temperature=
                  mod_som_efe_obp_ptr->fill_segment_ptr->avg_ctd_temperature;

              mod_som_efe_obp_ptr->cpt_spectra_ptr->avg_ctd_salinity=
                  mod_som_efe_obp_ptr->fill_segment_ptr->avg_ctd_salinity;

              mod_som_efe_obp_ptr->cpt_spectra_ptr->avg_ctd_fallrate  =
                  mod_som_efe_obp_ptr->fill_segment_ptr->avg_ctd_fallrate;


          tick=sl_sleeptimer_get_tick_count64();
          mystatus = sl_sleeptimer_tick64_to_ms(tick,\
                                                &mod_som_efe_obp_ptr->start_computation_timestamp);

          //CAP Add compute spectra functions
          mod_som_efe_obp_compute_spectra_data_f(curr_temp_seg_ptr,
                                                 curr_shear_seg_ptr,
                                                 curr_accel_seg_ptr);

          //ALB Make fake spectra in order to build the consumer while CAP
          //ALB is merging the obp functions.
          for (i=0;i<mod_som_efe_obp_ptr->settings_ptr->nfft/2;i++){
              mod_som_efe_obp_ptr->cpt_spectra_ptr->spec_temp_ptr[i]=
                    mod_som_efe_obp_ptr->fill_segment_ptr->seg_temp_volt_ptr[i];
              mod_som_efe_obp_ptr->cpt_spectra_ptr->spec_shear_ptr[i]=
                  mod_som_efe_obp_ptr->fill_segment_ptr->seg_shear_volt_ptr[i];
              mod_som_efe_obp_ptr->cpt_spectra_ptr->spec_accel_ptr[i]=
                  mod_som_efe_obp_ptr->fill_segment_ptr->seg_accel_volt_ptr[i];
          }

          //ALB update mod_som_efe_obp_ptr->producer_ptr->volt_read_index
          //ALB to get a new segment with a 50% overlap.
          mod_som_efe_obp_ptr->cpt_spectra_ptr->spectrum_cnt++;
          segment_avail = mod_som_efe_obp_ptr->fill_segment_ptr->segment_cnt -
              mod_som_efe_obp_ptr->cpt_spectra_ptr->spectrum_cnt; //elements available have been produced

          mod_som_efe_obp_ptr->fill_segment_ptr->segment_skipped=0;
          }
          //ALB done computing spectra
      }

      // Delay Start Task execution for
      OSTimeDly( MOD_SOM_EFE_OBP_CPT_SPECTRA_DELAY,             //   consumer delay is #define at the beginning OS Ticks
                 OS_OPT_TIME_DLY,          //   from now.
                 &err);
      //   Check error code.
      APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), ;);
//      printf("EFEOBP spec: %lu\r\n",(uint32_t) mod_som_efe_obp_ptr->cpt_spectra_ptr->spectrum_cnt);
  } // end of while (DEF_ON)

  PP_UNUSED_PARAM(p_arg);                                     // Prevent config warning.

}



/*******************************************************************************
 * @brief
 *   producer task function
 *
 *   this will be a state machine:
 *    3 - compute epsilon and chi
 *
 *   compute nu and kappa
 *   convert frequency to wavenumber
 *   sum Fourier coef
 *   get frequency cut-off
 *
 * @return
 *   MOD_SOM_STATUS_OK if initialization goes well
 *   or otherwise
 ******************************************************************************/
void mod_som_efe_obp_cpt_dissrate_task_f(void  *p_arg){
  RTOS_ERR  err;

  (void)p_arg; // Deliberately unused argument


  int i;
  int spectra_avail=0, reset_dissrates_cnt=0;
  int spectra_offset=0;
  int avg_spectra_offset=0;
  int padding = 0; // the padding should be big enough to include the time variance.

  float * curr_temp_spec_ptr;
  float * curr_shear_spec_ptr;
  float * curr_accel_spec_ptr;

  while (DEF_ON) {
      /************************************************************************/
      //ALB phase 3
      //ALB start computing epsilon chi.
      //ALB check first if spectra_ready_flg true: i.e.,do we have enough dof
      if (mod_som_efe_obp_ptr->cpt_dissrate_ptr->started_flg){
          spectra_avail = mod_som_efe_obp_ptr->cpt_spectra_ptr->spectrum_cnt -
              mod_som_efe_obp_ptr->cpt_dissrate_ptr->spectrum_cnt;  //calculate number of elements available have been produced
          // LOOP without delay until caught up to latest produced element
          //ALB segment_avail >1 because I am incrementing segment_cnt every helf nfft to get a 50% overlap
          while (spectra_avail > 0)
            {
              // When have circular buffer overflow: have produced data bigger than consumer data: 1 circular buffer (n_elmnts)
              // calculate new consumer count to skip ahead to the tail of the circular buffer (with optional padding),
              // calculate the number of data we skipped, report number of elements skipped.
              // Reset the consumers cnt equal with producer data plus padding

              if (spectra_avail>MOD_SOM_EFE_OBP_CPT_SPECTRA_NB_SPECTRA_PER_RECORD){ // checking over flow. TODO check adding padding is correct.
                  // reset the consumer count less one buffer than producer count plus padding
                  //ALB I think I want to change this line from the cb example. The "-" only works if you overflowed once.
                  reset_dissrates_cnt = mod_som_efe_obp_ptr->cpt_spectra_ptr->spectrum_cnt -
                                      MOD_SOM_EFE_OBP_CPT_SPECTRA_NB_SPECTRA_PER_RECORD +
                                      padding;
                  // calculate the number of skipped elements
                  mod_som_efe_obp_ptr->cpt_spectra_ptr->spectra_skipped = reset_dissrates_cnt -
                      mod_som_efe_obp_ptr->cpt_dissrate_ptr->dissrates_cnt;

                  mod_som_io_print_f("\n efe obp cpt dissrate: CB overflow: sample count = %lu,"
                      "cnsmr_cnt = %lu,skipped %lu elements \r\n ", \
                      (uint32_t)mod_som_efe_obp_ptr->cpt_spectra_ptr->spectrum_cnt, \
                      (uint32_t)mod_som_efe_obp_ptr->cpt_dissrate_ptr->dissrates_cnt, \
                      (uint32_t)mod_som_efe_obp_ptr->cpt_spectra_ptr->spectra_skipped);

                  mod_som_efe_obp_ptr->cpt_dissrate_ptr->dissrates_cnt = reset_dissrates_cnt;
              }

              //ALB calculate the offset for current pointer
              spectra_offset = (mod_som_efe_obp_ptr->cpt_dissrate_ptr->spectrum_cnt*
                                    mod_som_efe_obp_ptr->settings_ptr->nfft/2) %
                                   (MOD_SOM_EFE_OBP_CPT_SPECTRA_NB_SPECTRA_PER_RECORD*
                                    mod_som_efe_obp_ptr->settings_ptr->nfft/2);

              //ALB update the current temperature spectrum pointer
              curr_temp_spec_ptr   =
                 mod_som_efe_obp_ptr->cpt_spectra_ptr->spec_temp_ptr+
                 spectra_offset;

              //ALB update the current shear spectrum pointer
              curr_shear_spec_ptr   =
                  mod_som_efe_obp_ptr->cpt_spectra_ptr->spec_shear_ptr+
                  spectra_offset;

              //ALB update the current accel spectrum pointer
              curr_accel_spec_ptr   =
                  mod_som_efe_obp_ptr->cpt_spectra_ptr->spec_accel_ptr+
                  spectra_offset;

              //ALB update the current spectra timestamp (middle segment timestamp )
              mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_timestamp[
                          mod_som_efe_obp_ptr->cpt_dissrate_ptr->dissrates_cnt%
                           MOD_SOM_EFE_OBP_CPT_DISSRATE_NB_RATES_PER_RECORD]+=
                  mod_som_efe_obp_ptr->cpt_spectra_ptr->avg_timestamp/
                  (float)mod_som_efe_obp_ptr->settings_ptr->degrees_of_freedom;

              //ALB sum ctd temp,pressure/salinty
              mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_ctd_pressure+=
                  mod_som_efe_obp_ptr->cpt_spectra_ptr->avg_ctd_pressure/
                  (float)mod_som_efe_obp_ptr->settings_ptr->degrees_of_freedom;

              mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_ctd_salinity+=
                  mod_som_efe_obp_ptr->cpt_spectra_ptr->avg_ctd_salinity/
                  (float)mod_som_efe_obp_ptr->settings_ptr->degrees_of_freedom;

              mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_ctd_temperature+=
                  mod_som_efe_obp_ptr->cpt_spectra_ptr->avg_ctd_temperature/
                  (float)mod_som_efe_obp_ptr->settings_ptr->degrees_of_freedom;

              mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_ctd_fallrate+=
                  mod_som_efe_obp_ptr->cpt_spectra_ptr->avg_ctd_fallrate/
                  (float)mod_som_efe_obp_ptr->settings_ptr->degrees_of_freedom;


              //ALB sum spectra
              for (i=0;i<mod_som_efe_obp_ptr->settings_ptr->nfft/2;i++){

                  mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_spec_temp_ptr[
                                                         avg_spectra_offset+i]+=
                                                         curr_temp_spec_ptr[i]/
                   (float)mod_som_efe_obp_ptr->settings_ptr->degrees_of_freedom;

                  mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_spec_shear_ptr[
                                                         avg_spectra_offset+i]+=
                                                         curr_shear_spec_ptr[i]/
                   (float)mod_som_efe_obp_ptr->settings_ptr->degrees_of_freedom;

                  mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_spec_accel_ptr[
                                                         avg_spectra_offset+i]+=
                                                         curr_accel_spec_ptr[i]/
                   (float)mod_som_efe_obp_ptr->settings_ptr->degrees_of_freedom;
              }

              mod_som_efe_obp_ptr->cpt_dissrate_ptr->spectrum_cnt++;
              spectra_avail = mod_som_efe_obp_ptr->cpt_spectra_ptr->spectrum_cnt -
                            mod_som_efe_obp_ptr->cpt_dissrate_ptr->spectrum_cnt;

              if ((mod_som_efe_obp_ptr->cpt_dissrate_ptr->spectrum_cnt %
                    mod_som_efe_obp_ptr->settings_ptr->degrees_of_freedom)==0)
                  {
                  mod_som_efe_obp_ptr->cpt_dissrate_ptr->dof_flag=true;
              }



            }

          if (mod_som_efe_obp_ptr->cpt_dissrate_ptr->dof_flag)
            {
              //CAP Add compute epsilon chi
              //ALB Make fake epsilon and chi in order to build the consumer
              //ALB while CAP is merging the obp functions.

              for (i=0;i<mod_som_efe_obp_ptr->settings_ptr->nfft/2;i++){
                  //ALB Awfully complicated lines to get the value
                  //ALB of the current epsilon ptr and add the value of
                  //ALB the current avg temp spec. Just playing with my ptr skilssssss
                  *(mod_som_efe_obp_ptr->cpt_dissrate_ptr->epsilon +
                      (mod_som_efe_obp_ptr->cpt_dissrate_ptr->dissrates_cnt%
                          MOD_SOM_EFE_OBP_CPT_DISSRATE_NB_RATES_PER_RECORD)
                  )+=
                      *(mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_spec_shear_ptr+i);

                  //ALB Awfully complicated lines to get the value
                  //ALB of the current epsilon ptr and add the value of
                  //ALB the current avg temp spec.  Just playing with my ptr skilssssss
                  *(mod_som_efe_obp_ptr->cpt_dissrate_ptr->epsilon +
                      (mod_som_efe_obp_ptr->cpt_dissrate_ptr->dissrates_cnt%
                          MOD_SOM_EFE_OBP_CPT_DISSRATE_NB_RATES_PER_RECORD)
                  )+=
                      *(mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_spec_temp_ptr+i);
              }

              //ALB increment the counters
              mod_som_efe_obp_ptr->sample_count++;

              //get spectral offset
              avg_spectra_offset=(mod_som_efe_obp_ptr->cpt_dissrate_ptr->dissrates_cnt*
                  mod_som_efe_obp_ptr->settings_ptr->nfft/2) %
                      (MOD_SOM_EFE_OBP_CPT_DISSRATE_NB_AVG_SPECTRA_PER_RECORD*
                          mod_som_efe_obp_ptr->settings_ptr->nfft/2);
             //ALB reset  dof_flag;
              mod_som_efe_obp_ptr->cpt_dissrate_ptr->dof_flag=false;

              mod_som_efe_obp_ptr->cpt_dissrate_ptr->dissrates_cnt++;


          } //ALB end of dof if loop.
     } //end started flag

      // Delay Start Task execution for
      OSTimeDly( MOD_SOM_EFE_OBP_CPT_DISSRATE_DELAY,             //   consumer delay is #define at the beginning OS Ticks
                 OS_OPT_TIME_DLY,          //   from now.
                 &err);
      //   Check error code.
      APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), ;);
//      printf("EFEOBP dof: %lu\r\n",(uint32_t) mod_som_efe_obp_ptr->cpt_dissrate_ptr->spectrum_cnt %
//             mod_som_efe_obp_ptr->settings_ptr->degrees_of_freedom);
//      printf("EFEOBP diss: %lu\r\n",(uint32_t) mod_som_efe_obp_ptr->cpt_dissrate_ptr->dissrates_cnt);

 } // end of while (DEF_ON)

  PP_UNUSED_PARAM(p_arg);                                     // Prevent config warning.

}







/*******************************************************************************
 * @brief
 *   mod_som_efe_obp_cnt2volt_f
 *
 *   - Get a efe sample split in  nb_channels * ADC sample bytes
 *     default: 7 channels * 3 bytes
 *   - convert each ADC counts into volts. The conversion depends on the ADC config
 *     Unipolar = @(FR,data) (FR/gain*double(data)/2.^(bit_counts));
 *     Bipolar  = @(FR,data) (FR/gain*(double(data)/2.^(bit_counts-1)-1));
 *     FR = Full Range 2.5V
 *     bit counts: 24 bit
 *
 * @return
 *   MOD_SOM_STATUS_OK if initialization goes well
 *   or otherwise
 ******************************************************************************/

mod_som_status_t mod_som_efe_obp_cnt2volt_f(uint8_t * efe_sample,
                                            float   * local_efeobp_efe_temp_ptr,
                                            float   * local_efeobp_efe_shear_ptr,
                                            float   * local_efeobp_efe_accel_ptr)
{
uint32_t adc_sample=0;
float    adc_sample_volt=0;
uint32_t index=0;

  //ALB loop ing over each ADC sample and storing them in the seg_volt array.
  //ALB Since I am only storing 3 channels (1 temp, 1 shear,1 accell) I am checking
  //ALB which channels the user selected.
  for(int i=0;i<MOD_SOM_EFE_OBP_CHANNEL_NUMBER;i++)
    {
      index=mod_som_efe_obp_ptr->settings_ptr->channels_id[i]*AD7124_SAMPLE_LENGTH;
      adc_sample=0;
      adc_sample=     (uint32_t) (efe_sample[index]<<16 |
                                 efe_sample[index+1]<<8 |
                                 efe_sample[index+2]);
      switch(mod_som_efe_obp_ptr->efe_settings_ptr->sensors[i].registers.CONFIG_0){
        case 0x1e0: //unipolar
          adc_sample_volt  = MOD_SOM_EFE_OBP_FULL_RANGE/
                             MOD_SOM_EFE_OBP_GAIN*
                             adc_sample/
                             pow(2,MOD_SOM_EFE_OBP_ADC_BIT);
          break;
        case 0x9e0: //bipolar
          adc_sample_volt  = MOD_SOM_EFE_OBP_FULL_RANGE/
                             MOD_SOM_EFE_OBP_GAIN*
                             (adc_sample/
                             pow(2,MOD_SOM_EFE_OBP_ADC_BIT-1)-1);
          break;
      }

      //ALB t1 and t2 are idx 0 and 1 in the efe_channel_id
      //ALB Check efe_settings_ptr to convince your self.
      if ((mod_som_efe_obp_ptr->settings_ptr->channels_id[i]==0) |
          (mod_som_efe_obp_ptr->settings_ptr->channels_id[i]==1))
      {
          memcpy( local_efeobp_efe_temp_ptr,
                 &adc_sample_volt,
                  sizeof(float));
      }

      //ALB s1 and s2 are idx 2 and 3 in the efe_channel_id
      //ALB Check efe_settings_ptr to convince your self.
      if ((mod_som_efe_obp_ptr->settings_ptr->channels_id[i]==2) |
          (mod_som_efe_obp_ptr->settings_ptr->channels_id[i]==3))
      {
          memcpy( local_efeobp_efe_shear_ptr,
                 &adc_sample_volt,
                  sizeof(float));
      }

      //ALB a1, a2, a3 are idx 4, 5 and 6.
      //ALB Check efe_settings_ptr to convince your self.
      if ((mod_som_efe_obp_ptr->settings_ptr->channels_id[i]==4) |
          (mod_som_efe_obp_ptr->settings_ptr->channels_id[i]==5) |
          (mod_som_efe_obp_ptr->settings_ptr->channels_id[i]==6) )
      {
          memcpy( local_efeobp_efe_accel_ptr,
                 &adc_sample_volt,
                  sizeof(float));
      }
  }//end for loop nb_channel

  return mod_som_efe_obp_encode_status_f(MOD_SOM_STATUS_OK);
}

/*******************************************************************************
 * @brief
 *   Computes FFT from segments
 *   once the efe obp starts it fill segment array.
 *   We need to compute the FFT over the record (i.e. data received)
 *
 * @return
 *   MOD_SOM_STATUS_OK if initialization goes well
 *   or otherwise
 ******************************************************************************/
mod_som_status_t mod_som_efe_obp_compute_spectra_data_f(float * curr_temp_seg_ptr,
                                                        float * curr_shear_seg_ptr,
                                                        float * curr_accel_seg_ptr)
{
  //CAP
  return mod_som_efe_obp_encode_status_f(MOD_SOM_STATUS_OK);
}

/*******************************************************************************
 * @brief
 *   create consumer task
 *
 * @return
 *   MOD_SOM_STATUS_OK if initialization goes well
 *   or otherwise
 ******************************************************************************/
mod_som_status_t mod_som_efe_obp_start_consumer_task_f(){

  RTOS_ERR err;
  // Consumer Task 2

  if(!mod_som_efe_obp_ptr->fill_segment_ptr->started_flg){
      printf("EFE OBP not started\r\n");
      return (mod_som_efe_obp_ptr->status = mod_som_efe_obp_encode_status_f(MOD_SOM_EFE_OBP_NOT_STARTED));
  }

  switch(mod_som_efe_obp_ptr->consumer_ptr->mode){
    case segment:
      memcpy(mod_som_efe_obp_ptr->consumer_ptr->tag,
              MOD_SOM_EFE_OBP_CONSUMER_SEGMENT_TAG,
              MOD_SOM_EFE_OBP_TAG_LENGTH);
      break;
    case spectra:
      memcpy(mod_som_efe_obp_ptr->consumer_ptr->tag,
              MOD_SOM_EFE_OBP_CONSUMER_SPECTRA_TAG,
              MOD_SOM_EFE_OBP_TAG_LENGTH);
      break;
    case dissrate:
      memcpy(mod_som_efe_obp_ptr->consumer_ptr->tag,
              MOD_SOM_EFE_OBP_CONSUMER_RATE_TAG,
              MOD_SOM_EFE_OBP_TAG_LENGTH);
      break;
  }

  mod_som_efe_obp_ptr->consumer_ptr->segment_cnt  = 0;
  mod_som_efe_obp_ptr->consumer_ptr->spectrum_cnt = 0;
  mod_som_efe_obp_ptr->consumer_ptr->rates_cnt    = 0;
  mod_som_efe_obp_ptr->consumer_ptr->started_flg  = true;

  OSTaskCreate(&efe_obp_consumer_task_tcb,
                       "efe obp consumer task",
                       mod_som_efe_obp_consumer_task_f,
                       DEF_NULL,
                       MOD_SOM_EFE_OBP_CONSUMER_TASK_PRIO,
           &efe_obp_consumer_task_stk[0],
           (MOD_SOM_EFE_OBP_CONSUMER_TASK_STK_SIZE / 10u),
           MOD_SOM_EFE_OBP_CONSUMER_TASK_STK_SIZE,
           0u,
           0u,
           DEF_NULL,
           (OS_OPT_TASK_STK_CLR),
           &err);


 // Check error code
 APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), 1);
 if(RTOS_ERR_CODE_GET(err) != RTOS_ERR_NONE)
   return (mod_som_efe_obp_ptr->status = mod_som_efe_encode_status_f(MOD_SOM_EFE_OPB_STATUS_FAIL_TO_START_CONSUMER_TASK));

  return mod_som_efe_obp_encode_status_f(MOD_SOM_STATUS_OK);

}
/*******************************************************************************
 * @brief
 *   stop consumer task
 *
 * @return
 *   MOD_SOM_STATUS_OK if initialization goes well
 *   or otherwise
 ******************************************************************************/
mod_som_status_t mod_som_efe_obp_stop_consumer_task_f(){


  RTOS_ERR err;
  OSTaskDel(&efe_obp_consumer_task_tcb,
             &err);

  mod_som_efe_obp_ptr->started_flag=false;


  if(RTOS_ERR_CODE_GET(err) != RTOS_ERR_NONE)
    return (mod_som_efe_obp_ptr->status = mod_som_efe_encode_status_f(MOD_SOM_EFE_OPB_STATUS_FAIL_TO_START_CONSUMER_TASK));

  return mod_som_efe_encode_status_f(MOD_SOM_STATUS_OK);
}


/*******************************************************************************
 * @brief
 *   consumer task function
 *
 *   the user can choose between different mode of consumption
 *   0: none
 *   1: stream data (volt timeseries,volt spectra, epsi, chi)
 *   2: SDstore data (volt timeseries,volt spectra epsi, chi)
 *   3: stream and store (volt timeseries,volt spectra, epsi, chi)
 *   4: apf - The efe obp communicates with the apf module
 *
 *
 * @return
 *
 ******************************************************************************/
void mod_som_efe_obp_consumer_task_f(void  *p_arg){

  RTOS_ERR err;
  uint64_t segment_avail;
  uint64_t spectrum_avail;
  uint64_t rates_avail;

  uint32_t payload_length=0;
  int reset_segment_cnt=0;
  int reset_spectrum_cnt=0;
  int reset_rates_cnt=0;

  uint64_t tick;

  uint8_t * curr_consumer_record_ptr;

  //        printf("In Consumer Task 2\n");
  while (DEF_ON) {

      if (mod_som_efe_obp_ptr->consumer_ptr->started_flg &
          (mod_som_efe_obp_ptr->fill_segment_ptr->segment_cnt>=1)){


          switch(mod_som_efe_obp_ptr->consumer_ptr->mode){
            case segment:
              memcpy(mod_som_efe_obp_ptr->consumer_ptr->tag,
                     MOD_SOM_EFE_OBP_CONSUMER_SEGMENT_TAG,
                     MOD_SOM_EFE_OBP_TAG_LENGTH);

              //ALB phase 1: check elements available and copy them in the cnsmr buffer
              segment_avail=mod_som_efe_obp_ptr->fill_segment_ptr->half_segment_cnt-1-
                                mod_som_efe_obp_ptr->consumer_ptr->segment_cnt;

              if(segment_avail>0){
                  if (segment_avail>(2*MOD_SOM_EFE_OBP_FILL_SEGMENT_NB_SEGMENT_PER_RECORD)-1){
                      reset_segment_cnt =
                          mod_som_efe_obp_ptr->fill_segment_ptr->half_segment_cnt -
                          (2*MOD_SOM_EFE_OBP_FILL_SEGMENT_NB_SEGMENT_PER_RECORD) ;
                      // calculate the number of skipped elements
                      mod_som_efe_obp_ptr->consumer_ptr->elmnts_skipped =
                          reset_segment_cnt -
                          mod_som_efe_obp_ptr->consumer_ptr->segment_cnt;

                      mod_som_io_print_f("\n EFE OBP consumer task: "
                                         "segment overflow sample count = "
                                         "%lu,cnsmr_cnt = %lu,"
                                         "skipped %lu elements, ",
                           (uint32_t)mod_som_efe_obp_ptr->fill_segment_ptr->segment_cnt,
                           (uint32_t)mod_som_efe_obp_ptr->consumer_ptr->segment_cnt,
                           (uint32_t)mod_som_efe_obp_ptr->consumer_ptr->elmnts_skipped);

                      mod_som_efe_obp_ptr->consumer_ptr->segment_cnt =
                                                                  reset_segment_cnt;
                  }//ALB end deal with overflow


                  //ALB cpy the segments in the cnsmr buffer.
                  payload_length=mod_som_efe_obp_copy_producer_segment_f();
                  //ALB increase counter.
                  mod_som_efe_obp_ptr->consumer_ptr->segment_cnt++;
                  segment_avail=mod_som_efe_obp_ptr->fill_segment_ptr->segment_cnt-
                                    mod_som_efe_obp_ptr->consumer_ptr->segment_cnt;
            }
              break;

            case spectra:
              memcpy(mod_som_efe_obp_ptr->consumer_ptr->tag,
                     MOD_SOM_EFE_OBP_CONSUMER_SPECTRA_TAG,
                     MOD_SOM_EFE_OBP_TAG_LENGTH);
              //ALB phase 1: check elements available and copy them in the cnsmr buffer
              spectrum_avail=mod_som_efe_obp_ptr->cpt_spectra_ptr->spectrum_cnt-
                                mod_som_efe_obp_ptr->consumer_ptr->spectrum_cnt;
              if(spectrum_avail>0){
                  if (spectrum_avail>MOD_SOM_EFE_OBP_CPT_SPECTRA_NB_SPECTRA_PER_RECORD){
                      reset_spectrum_cnt =
                          mod_som_efe_obp_ptr->cpt_spectra_ptr->spectrum_cnt -
                          MOD_SOM_EFE_OBP_CPT_SPECTRA_NB_SPECTRA_PER_RECORD ;
                      // calculate the number of skipped elements
                      mod_som_efe_obp_ptr->consumer_ptr->elmnts_skipped =
                          reset_spectrum_cnt -
                          mod_som_efe_obp_ptr->consumer_ptr->spectrum_cnt;

                      mod_som_io_print_f("\n EFE OBP consumer task: "
                                         "spectra overflow sample count = "
                                         "%lu,cnsmr_cnt = %lu,"
                                         "skipped %lu elements, ",
                           (uint32_t)mod_som_efe_obp_ptr->cpt_spectra_ptr->spectrum_cnt,
                           (uint32_t)mod_som_efe_obp_ptr->consumer_ptr->spectrum_cnt,
                           (uint32_t)mod_som_efe_obp_ptr->consumer_ptr->elmnts_skipped);

                      mod_som_efe_obp_ptr->consumer_ptr->spectrum_cnt =
                                                                  reset_spectrum_cnt;
                  }//ALB end deal with overflow

                  //ALB cpy the segments in the cnsmr buffer.
                  payload_length=mod_som_efe_obp_copy_producer_spectra_f();
                  //ALB increase counter.
                  mod_som_efe_obp_ptr->consumer_ptr->spectrum_cnt++;
                  spectrum_avail=mod_som_efe_obp_ptr->cpt_spectra_ptr->spectrum_cnt-
                      mod_som_efe_obp_ptr->consumer_ptr->spectrum_cnt;
              }
              break;

            case dissrate:
              memcpy(mod_som_efe_obp_ptr->consumer_ptr->tag,
                     MOD_SOM_EFE_OBP_CONSUMER_SPECTRA_TAG,
                     MOD_SOM_EFE_OBP_TAG_LENGTH);
              //ALB phase 1: check elements available and copy them in the cnsmr buffer
              rates_avail=mod_som_efe_obp_ptr->cpt_dissrate_ptr->dissrates_cnt-
                                mod_som_efe_obp_ptr->consumer_ptr->rates_cnt;
              if(rates_avail>0){
                  if (rates_avail>MOD_SOM_EFE_OBP_CPT_DISSRATE_NB_RATES_PER_RECORD){
                      reset_rates_cnt =
                          mod_som_efe_obp_ptr->cpt_dissrate_ptr->dissrates_cnt -
                          MOD_SOM_EFE_OBP_CPT_DISSRATE_NB_RATES_PER_RECORD ;
                      // calculate the number of skipped elements
                      mod_som_efe_obp_ptr->consumer_ptr->elmnts_skipped =
                          reset_rates_cnt -
                          mod_som_efe_obp_ptr->consumer_ptr->rates_cnt;

                      mod_som_io_print_f("\n EFE OBP consumer task: "
                                         "rates overflow sample count = "
                                         "%lu,cnsmr_cnt = %lu,"
                                         "skipped %lu elements, ",
                           (uint32_t)mod_som_efe_obp_ptr->cpt_dissrate_ptr->dissrates_cnt,
                           (uint32_t)mod_som_efe_obp_ptr->consumer_ptr->rates_cnt,
                           (uint32_t)mod_som_efe_obp_ptr->consumer_ptr->elmnts_skipped);

                      mod_som_efe_obp_ptr->consumer_ptr->rates_cnt =
                                                                  reset_rates_cnt;
                  }//ALB end deal with overflow


                  //ALB cpy the segments in the cnsmr buffer.
                  payload_length=mod_som_efe_obp_copy_producer_dissrate_f();

                  mod_som_efe_obp_ptr->consumer_ptr->rates_cnt++;
                  rates_avail=mod_som_efe_obp_ptr->cpt_dissrate_ptr->dissrates_cnt-
                                    mod_som_efe_obp_ptr->consumer_ptr->rates_cnt;

              }
              break;
          }



          if(payload_length>0){


          //get the timestamp for the record header
          tick=sl_sleeptimer_get_tick_count64();
          mystatus = sl_sleeptimer_tick64_to_ms(tick,\
                           &mod_som_efe_obp_ptr->consumer_ptr->record_timestamp);

          //MHA: Now augment timestamp by poweron_offset_ms
          mod_som_calendar_settings=mod_som_calendar_get_settings_f(); //get the calendar settings pointer
          mod_som_efe_obp_ptr->consumer_ptr->record_timestamp +=
                                    mod_som_calendar_settings.poweron_offset_ms;


          mod_som_efe_obp_ptr->consumer_ptr->payload_length=payload_length;

          //ALB create header
          mod_som_efe_obp_header_f(mod_som_efe_obp_ptr->consumer_ptr);
          //add header to the beginning of the stream block
          memcpy(mod_som_efe_obp_ptr->consumer_ptr->record_ptr, \
                 mod_som_efe_obp_ptr->consumer_ptr->header,
                 mod_som_efe_obp_ptr->consumer_ptr->length_header);


          //ALB compute checksum
          curr_consumer_record_ptr=mod_som_efe_obp_ptr->consumer_ptr->record_ptr+
              mod_som_efe_obp_ptr->consumer_ptr->length_header;
          mod_som_efe_obp_ptr->consumer_ptr->chksum=0;
          for(int i=0;i<mod_som_efe_obp_ptr->consumer_ptr->payload_length;i++)
            {
              mod_som_efe_obp_ptr->consumer_ptr->chksum ^=\
                  curr_consumer_record_ptr[i];
            }


          //ALB the curr_consumer_element_ptr should be at the right place to
          //ALB write checksum at the end of the record.
          curr_consumer_record_ptr+=
                              mod_som_efe_obp_ptr->consumer_ptr->payload_length;
          *(curr_consumer_record_ptr++) = '*';
          *((uint16_t*)curr_consumer_record_ptr) = \
              mod_som_int8_2hex_f(mod_som_efe_obp_ptr->consumer_ptr->chksum);
          curr_consumer_record_ptr += 2;
          *(curr_consumer_record_ptr++) = '\r';
          *(curr_consumer_record_ptr++) = '\n';

          //ALB get the length of the record with the checksum
          mod_som_efe_obp_ptr->consumer_ptr->record_length= \
              (int) &curr_consumer_record_ptr[0]- \
              (int) &mod_som_efe_obp_ptr->consumer_ptr->record_ptr[0];



          switch(mod_som_efe_obp_ptr->mode){
            case 0:
              //ALB stream
              mod_som_efe_obp_ptr->consumer_ptr->consumed_flag=false;
              mod_som_io_stream_data_f(
                  mod_som_efe_obp_ptr->consumer_ptr->record_ptr,
                  mod_som_efe_obp_ptr->consumer_ptr->record_length,
                  &mod_som_efe_obp_ptr->consumer_ptr->consumed_flag);

              break;
            case 1:
              //ALB store
              mod_som_efe_obp_ptr->consumer_ptr->consumed_flag=false;
              mod_som_sdio_write_data_f(
                  mod_som_efe_obp_ptr->consumer_ptr->record_ptr,
                  mod_som_efe_obp_ptr->consumer_ptr->record_length,
                  &mod_som_efe_obp_ptr->consumer_ptr->consumed_flag);

              break;
            case 2:
              //ALB stream and store
              break;

          }//end switch efe_obp mode
          payload_length=0;
          }// end if payload length =0
  }//end if mod_som_efe_obp_ptr->consumer_ptr->started_flg
  // Delay Start Task execution for
  OSTimeDly( MOD_SOM_EFE_OBP_CONSUMER_DELAY,             //   consumer delay is #define at the beginning OS Ticks
             OS_OPT_TIME_DLY,          //   from now.
             &err);
  //   Check error code.
  APP_RTOS_ASSERT_DBG((RTOS_ERR_CODE_GET(err) == RTOS_ERR_NONE), ;);
} // end of while (DEF_ON)

PP_UNUSED_PARAM(p_arg);                                     // Prevent config warning.


}//end consumer task

/***************************************************************************//**
 * @brief
 *   build header data
 *   TODO add different flag as parameters like
 *        - data overlap
 *        -
 *   TODO
 ******************************************************************************/
void mod_som_efe_obp_header_f(mod_som_efe_obp_data_consumer_ptr_t consumer_ptr)
{

  //time stamp
  uint32_t t_hex[2];
  uint8_t * local_header;


  t_hex[0] = (uint32_t) (mod_som_efe_obp_ptr->consumer_ptr->record_timestamp>>32);
  t_hex[1] = (uint32_t) mod_som_efe_obp_ptr->consumer_ptr->record_timestamp;

  //header  contains $EFE,flags. Currently flags are hard coded to 0x1e
  //time stamp will come at the end of header
  sprintf((char*) mod_som_efe_obp_ptr->consumer_ptr->header,  \
      "$%.4s%08x%08x%08x*FF", \
      mod_som_efe_obp_ptr->consumer_ptr->tag, \
      (int) t_hex[0],\
      (int) t_hex[1],\
      (int) consumer_ptr->payload_length);

  consumer_ptr->header_chksum=0;
  for(int i=0;i<consumer_ptr->length_header-
               MOD_SOM_EFE_OBP_LENGTH_HEADER_CHECKSUM;i++) // 29 = sync char(1)+ tag (4) + hextimestamp (16) + payload size (8).
    {
      consumer_ptr->header_chksum ^=\
          consumer_ptr->header[i];
    }


  // the curr_consumer_element_ptr should be at the right place to
  // write the checksum already
  //write checksum at the end of the steam block (record).
  local_header = &consumer_ptr->header[consumer_ptr->length_header-
                                       MOD_SOM_EFE_OBP_LENGTH_HEADER_CHECKSUM+1];
  *((uint16_t*)local_header) = \
      mod_som_int8_2hex_f(consumer_ptr->header_chksum);

}


  /*******************************************************************************
   * @brief
   * ALB  copy segments in the cnsmr buffer segment_ptr
   * ALB  The plan so far is to store only 1 segment and consume it right after.
   * ALB  We start to fill the segment at the end of the header.
   * ALB  Once the memcopy is done I compute the checksum.
   *
   * @return
   *   MOD_SOM_STATUS_OK if initialization goes well
   *   or otherwise
   ******************************************************************************/

uint32_t mod_som_efe_obp_copy_producer_segment_f()
  {
  uint32_t indx=mod_som_efe_obp_ptr->config_ptr->header_length;
    //copy the prdcr segment inside cnsmr segment.


  memcpy((void *) &mod_som_efe_obp_ptr->consumer_ptr->record_ptr[indx],
         (void *) &mod_som_efe_obp_ptr->fill_segment_ptr->timestamp_segment,
                  sizeof(uint64_t));

  indx+=sizeof(uint64_t);

  switch(mod_som_efe_obp_ptr->consumer_ptr->channel){
    case temp:
      memcpy(&mod_som_efe_obp_ptr->consumer_ptr->record_ptr[indx],
             &mod_som_efe_obp_ptr->fill_segment_ptr->seg_temp_volt_ptr[
            (mod_som_efe_obp_ptr->consumer_ptr->segment_cnt%
             (2*MOD_SOM_EFE_OBP_FILL_SEGMENT_NB_SEGMENT_PER_RECORD))
           * mod_som_efe_obp_ptr->settings_ptr->nfft/2],
             mod_som_efe_obp_ptr->settings_ptr->nfft*sizeof(float));

      break;
    case shear:
      memcpy(&mod_som_efe_obp_ptr->consumer_ptr->record_ptr[indx],
             &mod_som_efe_obp_ptr->fill_segment_ptr->seg_shear_volt_ptr[
            (mod_som_efe_obp_ptr->consumer_ptr->segment_cnt%
             (2*MOD_SOM_EFE_OBP_FILL_SEGMENT_NB_SEGMENT_PER_RECORD))
           * mod_som_efe_obp_ptr->settings_ptr->nfft/2],
             mod_som_efe_obp_ptr->settings_ptr->nfft*sizeof(float));
      break;
    case accel:
      memcpy(&mod_som_efe_obp_ptr->consumer_ptr->record_ptr[indx],
             &mod_som_efe_obp_ptr->fill_segment_ptr->seg_accel_volt_ptr[
             (mod_som_efe_obp_ptr->consumer_ptr->segment_cnt%
             (2*MOD_SOM_EFE_OBP_FILL_SEGMENT_NB_SEGMENT_PER_RECORD))
            * mod_som_efe_obp_ptr->settings_ptr->nfft/2],
              mod_som_efe_obp_ptr->settings_ptr->nfft*sizeof(float));
      break;
  }

    //ALB return the length of the payload
    indx+=mod_som_efe_obp_ptr->settings_ptr->nfft*sizeof(float);
    indx-=mod_som_efe_obp_ptr->config_ptr->header_length;
     return indx;
  }

  /*******************************************************************************
   * @brief
   *   copy spectra in the cnsmr buffer
   *
   *
   * @return
   *   MOD_SOM_STATUS_OK if initialization goes well
   *   or otherwise
   ******************************************************************************/

  uint32_t mod_som_efe_obp_copy_producer_spectra_f()
  {
    uint32_t indx=mod_som_efe_obp_ptr->config_ptr->header_length;
    //copy the prdcr spectra inside cnsmr spectra buffer.


    memcpy((void *) &mod_som_efe_obp_ptr->consumer_ptr->record_ptr[indx],
           (void *) &mod_som_efe_obp_ptr->cpt_spectra_ptr->avg_timestamp,
                    sizeof(uint64_t));

    indx+=sizeof(uint64_t);

      switch(mod_som_efe_obp_ptr->consumer_ptr->channel){
    case temp:

    memcpy(&mod_som_efe_obp_ptr->consumer_ptr->record_ptr[indx],
           &mod_som_efe_obp_ptr->cpt_spectra_ptr->spec_temp_ptr[
                      (mod_som_efe_obp_ptr->consumer_ptr->spectrum_cnt%
                       MOD_SOM_EFE_OBP_CPT_SPECTRA_NB_SPECTRA_PER_RECORD)
                       * mod_som_efe_obp_ptr->settings_ptr->nfft/2],
           mod_som_efe_obp_ptr->settings_ptr->nfft/2*sizeof(float));
    break;
    case shear:
      memcpy(&mod_som_efe_obp_ptr->consumer_ptr->record_ptr[indx],
             &mod_som_efe_obp_ptr->cpt_spectra_ptr->spec_shear_ptr[
                        (mod_som_efe_obp_ptr->consumer_ptr->spectrum_cnt%
                         MOD_SOM_EFE_OBP_CPT_SPECTRA_NB_SPECTRA_PER_RECORD)
                         * mod_som_efe_obp_ptr->settings_ptr->nfft/2],
             mod_som_efe_obp_ptr->settings_ptr->nfft/2*sizeof(float));
      break;
    case accel:
      memcpy(&mod_som_efe_obp_ptr->consumer_ptr->record_ptr[indx],
             &mod_som_efe_obp_ptr->cpt_spectra_ptr->spec_accel_ptr[
                        (mod_som_efe_obp_ptr->consumer_ptr->spectrum_cnt%
                         MOD_SOM_EFE_OBP_CPT_SPECTRA_NB_SPECTRA_PER_RECORD)
                         * mod_som_efe_obp_ptr->settings_ptr->nfft/2],
             mod_som_efe_obp_ptr->settings_ptr->nfft/2*sizeof(float));
      break;
      }

    //ALB return the length of the payload
    indx+=mod_som_efe_obp_ptr->settings_ptr->nfft/2*sizeof(float);
    indx-=mod_som_efe_obp_ptr->config_ptr->header_length;
     return indx;
  }


  /*******************************************************************************
   * @brief
   *   copy spectra in the cnsmr buffer
   *
   *
   * @return
   *   MOD_SOM_STATUS_OK if initialization goes well
   *   or otherwise
   ******************************************************************************/

  uint32_t mod_som_efe_obp_copy_producer_dissrate_f()
  {
    uint32_t indx=mod_som_efe_obp_ptr->config_ptr->header_length;
    //copy the prdcr spectra inside cnsmr spectra buffer.
    memcpy(&mod_som_efe_obp_ptr->consumer_ptr->record_ptr[indx],
           &mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_spec_temp_ptr[
                      (mod_som_efe_obp_ptr->consumer_ptr->rates_cnt%
                       MOD_SOM_EFE_OBP_CPT_SPECTRA_NB_SPECTRA_PER_RECORD)
                       * mod_som_efe_obp_ptr->settings_ptr->nfft/2],
           mod_som_efe_obp_ptr->settings_ptr->nfft/2);

    indx+=mod_som_efe_obp_ptr->settings_ptr->nfft/2;
    memcpy(&mod_som_efe_obp_ptr->consumer_ptr->record_ptr[indx],
           &mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_spec_shear_ptr[
                      (mod_som_efe_obp_ptr->consumer_ptr->rates_cnt%
                       MOD_SOM_EFE_OBP_CPT_SPECTRA_NB_SPECTRA_PER_RECORD)
                       * mod_som_efe_obp_ptr->settings_ptr->nfft/2],
           mod_som_efe_obp_ptr->settings_ptr->nfft/2);

    indx+=mod_som_efe_obp_ptr->settings_ptr->nfft/2;
    memcpy(&mod_som_efe_obp_ptr->consumer_ptr->record_ptr[indx],
           &mod_som_efe_obp_ptr->cpt_dissrate_ptr->avg_spec_accel_ptr[
                      (mod_som_efe_obp_ptr->consumer_ptr->rates_cnt%
                       MOD_SOM_EFE_OBP_CPT_SPECTRA_NB_SPECTRA_PER_RECORD)
                       * mod_som_efe_obp_ptr->settings_ptr->nfft/2],
           mod_som_efe_obp_ptr->settings_ptr->nfft/2);

    indx+=mod_som_efe_obp_ptr->settings_ptr->nfft/2;
    memcpy(&mod_som_efe_obp_ptr->consumer_ptr->record_ptr[indx],
           &mod_som_efe_obp_ptr->cpt_dissrate_ptr->chi[
                      (mod_som_efe_obp_ptr->consumer_ptr->rates_cnt%
                       MOD_SOM_EFE_OBP_CPT_SPECTRA_NB_SPECTRA_PER_RECORD)
                       ],sizeof(float));
    indx+=sizeof(float);
    memcpy(&mod_som_efe_obp_ptr->consumer_ptr->record_ptr[indx],
           &mod_som_efe_obp_ptr->cpt_dissrate_ptr->epsilon[
                      (mod_som_efe_obp_ptr->consumer_ptr->rates_cnt%
                       MOD_SOM_EFE_OBP_CPT_SPECTRA_NB_SPECTRA_PER_RECORD)
                       ],sizeof(float));

    indx+=sizeof(float);
    memcpy(&mod_som_efe_obp_ptr->consumer_ptr->record_ptr[indx],
           &mod_som_efe_obp_ptr->cpt_dissrate_ptr->nu[
                      (mod_som_efe_obp_ptr->consumer_ptr->rates_cnt%
                       MOD_SOM_EFE_OBP_CPT_SPECTRA_NB_SPECTRA_PER_RECORD)
                       ],sizeof(float));

    indx+=sizeof(float);
    memcpy(&mod_som_efe_obp_ptr->consumer_ptr->record_ptr[indx],
           &mod_som_efe_obp_ptr->cpt_dissrate_ptr->kappa[
                      (mod_som_efe_obp_ptr->consumer_ptr->rates_cnt%
                       MOD_SOM_EFE_OBP_CPT_SPECTRA_NB_SPECTRA_PER_RECORD)
                       ],sizeof(float));

    indx+=sizeof(float);
    indx-=mod_som_efe_obp_ptr->config_ptr->header_length;
    //ALB return the length of the payload
     return indx;
  }


// ALB command functions //SN edited to be functions called in shell //SN need to edit comments
/*******************************************************************************
 * @brief
 *   change efeobp.mode  for the 'efeobp.mode' command.
 ******************************************************************************/
mod_som_status_t mod_som_efe_obp_consumer_mode_f(CPU_INT16U argc,CPU_CHAR *argv[])
{

  RTOS_ERR  p_err;
  uint8_t mode;

  if (argc==1){
      printf("efeobp.mode %u\r\n.", mod_som_efe_obp_ptr->consumer_ptr->mode);
  }
  else{
    //ALB switch statement easy to handle all user input cases.
    switch (argc){
    case 2:
      mode=shellStrtol(argv[1],&p_err);
      if(mode<3){
          mod_som_efe_obp_ptr->consumer_ptr->mode=mode;
      }else{
          printf("format: efeobp.mode mode (0:segment, 1:spectra, 2: dissrates)\r\n");
      }
      break;
    default:
      printf("format: efeobp.mode mode (0:segment, 1:spectra, 2: dissrates)\r\n");
      break;
    }
  }

  return mod_som_efe_encode_status_f(MOD_SOM_STATUS_OK);
}

// ALB command functions //SN edited to be functions called in shell //SN need to edit comments
/*******************************************************************************
 * @brief
 *   change efeobp.mode  for the 'efeobp.mode' command.
 ******************************************************************************/
mod_som_status_t mod_som_efe_obp_consumer_channel_f(CPU_INT16U argc,CPU_CHAR *argv[])
{

  RTOS_ERR  p_err;
  uint8_t channel;

  if (argc==1){
      printf("efeobp.channel %u\r\n.", mod_som_efe_obp_ptr->consumer_ptr->channel);
  }
  else{
    //ALB switch statement easy to handle all user input cases.
    switch (argc){
    case 2:
      channel=shellStrtol(argv[1],&p_err);
      if(channel<3){
          mod_som_efe_obp_ptr->consumer_ptr->channel=channel;
      }else{
          printf("format: efeobp.channel (0:temperature, 1:shear, 2: acceleration)\r\n");
      }
      break;
    default:
      printf("format: efeobp.channel (0:temperature, 1:shear, 2: acceleration)\r\n");
      break;
    }
  }

  return mod_som_efe_encode_status_f(MOD_SOM_STATUS_OK);
}






/*******************************************************************************
 * @function
 *     mod_som_io_decode_status_f
 * @abstract
 *     Decode mod_som_status to status of MOD SOM I/O error codes
 * @discussion TODO SN
 *     The status is system wide, so we only decode the bit 16-23 if the
 *     higher bits show the status code is of MOD SOM FOO BAR
 * @param       mod_som_status
 *     MOD SOM general system-wide status
 * @return
 *     0xff if non-MOD SOM I/O status, otherwise status, describes in general
 *     header file
 ******************************************************************************/
uint8_t mod_som_efe_obp_decode_status_f(mod_som_status_t mod_som_status){
    if(mod_som_status==MOD_SOM_STATUS_OK)
        return MOD_SOM_STATUS_OK;
    uint8_t status_prefix;
    uint8_t decoded_status;
    MOD_SOM_DECODE_STATUS(mod_som_status,&status_prefix,&decoded_status);
    if(status_prefix != MOD_SOM_EFE_OBP_STATUS_PREFIX){
        return 0xffU;
    }
    return decoded_status;
}

/*******************************************************************************
 * @function
 *     mod_som_efe_obp_encode_status_f
 * @abstract
 *     Encode status of MOD SOM I/O error code to MOD SOM general status
 * @discussion TODO SN
 *     Encode status of MOD SOM IO status code to MOD SOM general status, where
 *     top 8 bits are module identifier, bit 16-23 store 8-bit status code,
 *     lower 16 bits are reserved for flags
 * @param       mod_som_io_status
 *     MOD SOM I/O error code
 * @return
 *     MOD SOM status code
 ******************************************************************************/
mod_som_status_t mod_som_efe_obp_encode_status_f(uint8_t mod_som_io_status){
    if(mod_som_io_status==MOD_SOM_STATUS_OK)
        return MOD_SOM_STATUS_OK;
    return MOD_SOM_ENCODE_STATUS(MOD_SOM_EFE_OBP_STATUS_PREFIX, mod_som_io_status);
}

