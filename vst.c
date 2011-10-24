/*
   Copyright (C) 2002-2003 Kjetil S. Matheussen / Notam

   V0.1.6 - stable.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

   As an exception to the GPL, you may distribute binary copies of
   this library that link to the VST SDK.

 */

#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>

#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <ladspa.h>
#include <vstlib.h>


#define VSTL_MAX(a,b) (((a)>(b))?(a):(b))
#define VSTL_MIN(a,b) (((a)<(b))?(a):(b))

#define BUFFERSIZE 1024 // There is no point in increasing this number unless you increase the BUFFERSIZE constant in the vstserver as well.

#define INTERVAL 1024 // Parameters are updated 1024 times a second.


struct VSTLadspa{
  AEffect *effect;
  int num_ports;

  LADSPA_Data **ports;
  LADSPA_Data *ports_compaire; // Backup to see if a port have changed its value.

  int blocksize;    // Current blocksize
  int newblocksize; // Requested blocksize. (Only used if veryrealtime==true)

  bool stopthatthread;
  pthread_t parameter_thread;

  pthread_mutex_t stopstartparameterthreadmutex;
 
  bool veryrealtime; // Is true if plugin must be RT safe.

  bool RTstopthatthread;
  pthread_t RTrunthread;

  pthread_cond_t RTcond;
  pthread_mutex_t RTmutex;

  unsigned long RTSampleCount;
  float **RTbuffer[2];
  int RTreadbuffer; // Either 0 or 1. The RTbuffer that was last processed.

};


static int num_plugins=0;
static LADSPA_Descriptor *VSTDescriptors[100000];


const LADSPA_Descriptor *ladspa_descriptor(unsigned long index) {
  if(index>=num_plugins) return NULL;

  return VSTDescriptors[index];
}


static void *VSTL_setParameterThread(void *arg){
  struct VSTLadspa *vstl=(struct VSTLadspa *)arg;
  AEffect *effect=vstl->effect;
  int lokke;

  while(vstl->stopthatthread==false){

    if(vstl->veryrealtime==true){
      if(vstl->newblocksize > vstl->blocksize){
	int lokke2;

	pthread_mutex_lock(&vstl->stopstartparameterthreadmutex);

	effect->dispatcher(effect,
			   effMainsChanged,
			   0, 0, NULL, 0);
	effect->dispatcher(effect,
			   effSetBlockSize,
			   0, vstl->newblocksize, NULL, 0);
	effect->dispatcher(effect,
			   effMainsChanged,
			   0, 0, NULL, 0);

	pthread_mutex_unlock(&vstl->stopstartparameterthreadmutex);

	for(lokke2=0;lokke2<2;lokke2++){
	  for(lokke=0;lokke<VSTL_MAX(effect->numInputs,effect->numOutputs);lokke++){
	    free(vstl->RTbuffer[lokke2][lokke]);
	    vstl->RTbuffer[lokke2][lokke]=calloc(sizeof(float),vstl->newblocksize);
	  }
	}

	vstl->blocksize=vstl->newblocksize;
      }
    }

    for(lokke=effect->numInputs+effect->numOutputs;lokke<vstl->num_ports -1;lokke++){
      if(vstl->ports[lokke]!=NULL){
	if(*(vstl->ports[lokke])!=vstl->ports_compaire[lokke]){
	  vstl->ports_compaire[lokke]=*(vstl->ports[lokke]);
	  pthread_mutex_lock(&vstl->stopstartparameterthreadmutex);
	  effect->setParameter(
			       effect,
			       lokke - (effect->numInputs+effect->numOutputs),
			       vstl->ports_compaire[lokke]
			       );
	  pthread_mutex_unlock(&vstl->stopstartparameterthreadmutex);
	}
      }
    }

    if(vstl->ports[lokke]!=NULL){
      if(*(vstl->ports[lokke])!=vstl->ports_compaire[lokke]){
	vstl->ports_compaire[lokke]=*(vstl->ports[lokke]);
	pthread_mutex_lock(&vstl->stopstartparameterthreadmutex);
	if(vstl->ports_compaire[lokke]<=0.0f){
	  effect->dispatcher(effect,
			     effEditClose,
			     0, 0, NULL, 0
			     );
	}else{
	  effect->dispatcher(effect,
			     effEditOpen,
			     0, 0, NULL, 0
			     );
	}
	pthread_mutex_unlock(&vstl->stopstartparameterthreadmutex);
      }
    }



    usleep(1000000/INTERVAL);
  }

  return NULL;
}

static int VSTL_startParameterthread(struct VSTLadspa *vstl){
  vstl->stopthatthread=false;
  return pthread_create(&vstl->parameter_thread,NULL,VSTL_setParameterThread,vstl);
}

static void VSTL_stopParameterthread(struct VSTLadspa *vstl){
  vstl->stopthatthread=true;
  pthread_join(vstl->parameter_thread,NULL);
}




static void *VSTL_RTrunthread(void *args){
  struct VSTLadspa *vstl=(struct VSTLadspa *)args;
  AEffect *effect=vstl->effect;
  int notRTreadbuffer;
  unsigned long SampleCount;

  for(;;){
    pthread_cond_wait(&vstl->RTcond,&vstl->RTmutex);
    if(vstl->RTstopthatthread==true) break;

    SampleCount=vstl->RTSampleCount;
    if(vstl->blocksize >= SampleCount){
      notRTreadbuffer=vstl->RTreadbuffer==0?1:0;
      effect->processReplacing(
			       effect,
			       vstl->RTbuffer[notRTreadbuffer],
			       vstl->RTbuffer[notRTreadbuffer],
			       SampleCount
			       );
      //      fprintf(stderr,"Prosessing\n");
      vstl->RTreadbuffer=notRTreadbuffer;
    }
  }

  return NULL;
}


static void VSTL_RTrun(
		       LADSPA_Handle Instance,
		       unsigned long SampleCount
		       )
{
  int lokke;
  struct VSTLadspa *vstl=(struct VSTLadspa *)Instance;
  AEffect *effect=vstl->effect;

  if(vstl->blocksize >= SampleCount){
    int notRTreadbuffer=vstl->RTreadbuffer==0?1:0;

    for(lokke=0;lokke<effect->numInputs;lokke++){
      memcpy(vstl->RTbuffer[notRTreadbuffer][lokke],vstl->ports[lokke],SampleCount*sizeof(float));
    }

    for(lokke=effect->numInputs;lokke<effect->numInputs+effect->numOutputs;lokke++){
      memcpy(vstl->ports[lokke],vstl->RTbuffer[vstl->RTreadbuffer][lokke - effect->numInputs],SampleCount*sizeof(float));
    }

    vstl->RTSampleCount=SampleCount;
    pthread_cond_broadcast(&vstl->RTcond);
    //    fprintf(stderr,"Requesting a process\n");


  }else{

    vstl->newblocksize=SampleCount;

    for(lokke=effect->numInputs;lokke<effect->numInputs+effect->numOutputs;lokke++){
      
#if 1
      // Silence output only.
      memset(vstl->ports[lokke],0,sizeof(float)*SampleCount);
#else
      // Make noise. For debugging.
      int lokke2;
      for(lokke2=0;lokke2<SampleCount;lokke2++){
	vstl->ports[lokke][lokke2]=(float)rand()/(float)RAND_MAX;
      }
#endif
    }
  }
}



static void VSTL_run(
	      LADSPA_Handle Instance,
              unsigned long sampleframes
	      )
{
  struct VSTLadspa *vstl=(struct VSTLadspa *)Instance;
  AEffect *effect=vstl->effect;
  int dasframes=sampleframes; // Need a signed variable.

  int ch;
  float *new_inputs[effect->numInputs];
  float *new_outputs[effect->numOutputs];

  for(ch=0;ch<effect->numInputs;ch++){
    new_inputs[ch]=vstl->ports[ch];
  }
  for(ch=0;ch<effect->numOutputs;ch++){
    new_outputs[ch]=vstl->ports[effect->numInputs + ch];
  }

  for(;;){
    effect->processReplacing(
			     effect,
			     new_inputs,
			     new_outputs,
			     VSTL_MIN(dasframes,BUFFERSIZE)
			     );
    
    dasframes-=BUFFERSIZE;
    if(dasframes<=0) break;

    for(ch=0;ch<effect->numInputs;ch++){
      new_inputs[ch]+=BUFFERSIZE;
    }
    for(ch=0;ch<effect->numOutputs;ch++){
      new_outputs[ch]+=BUFFERSIZE;
    }
  }
}




static LADSPA_Handle VSTL_instantiate(
			       const struct _LADSPA_Descriptor * Descriptor,
                               unsigned long                     SampleRate
			       )
{
  struct VSTLadspa *vstl;
  AEffect *effect;
  int lokke;

  effect=VSTLIB_new((char *)(Descriptor->Label+4));

  if(effect==NULL) return NULL;

  effect->dispatcher(effect,
		     effOpen,
		     0, 0, NULL, 0);


  effect->dispatcher(
		     effect,
		     effSetSampleRate,
		     0,0,NULL,(float)SampleRate);


  vstl=calloc(1,sizeof(struct VSTLadspa));
  vstl->blocksize=BUFFERSIZE;
  effect->dispatcher(effect,
		     effSetBlockSize,
		     0, vstl->blocksize, NULL, 0);


  vstl->effect=effect;
  vstl->num_ports=effect->numInputs+effect->numOutputs+effect->numParams + 1; // The last one is gui on/off
  vstl->ports=calloc(sizeof(LADSPA_Data*),vstl->num_ports);
  vstl->ports_compaire=calloc(sizeof(LADSPA_Data),vstl->num_ports);

  vstl->veryrealtime=Descriptor->run==VSTL_run?false:true;

  pthread_mutex_init(&vstl->stopstartparameterthreadmutex,NULL);

  if(vstl->veryrealtime==true){
    int lokke2;

    for(lokke2=0;lokke2<2;lokke2++){
      vstl->RTbuffer[lokke2]=calloc(sizeof(LADSPA_Data*),VSTL_MAX(effect->numInputs,effect->numOutputs));
      for(lokke=0;lokke<VSTL_MAX(effect->numInputs,effect->numOutputs);lokke++){
	vstl->RTbuffer[lokke2][lokke]=calloc(sizeof(float),vstl->blocksize);
      }
    }

    pthread_mutex_init(&vstl->RTmutex,NULL);
    pthread_cond_init(&vstl->RTcond,NULL);

    //printf("Plugin is realtime\n");
  }



  effect->dispatcher(effect,
		     effMainsChanged,
		     0, 1, NULL, 0);


  for(lokke=effect->numInputs+effect->numOutputs;lokke<vstl->num_ports -1;lokke++){
    vstl->ports_compaire[lokke]=effect->getParameter(
						     effect,
						     lokke - (effect->numInputs+effect->numOutputs)
						     );
  }
  vstl->ports_compaire[lokke]=0.0f;

  if(VSTL_startParameterthread(vstl)!=0) return NULL;

  vstl->RTstopthatthread=false;
  pthread_create(&vstl->RTrunthread,NULL,VSTL_RTrunthread,vstl);


  return vstl;
}


static void VSTL_cleanup(
		  LADSPA_Handle Instance
		  )
{
  struct VSTLadspa *vstl=(struct VSTLadspa *)Instance;
  AEffect *effect=vstl->effect;

  if(vstl->veryrealtime==true){
    int lokke,lokke2;
    vstl->RTstopthatthread=true;
    pthread_cond_broadcast(&vstl->RTcond);
    pthread_join(vstl->RTrunthread,NULL);

    for(lokke2=0;lokke2<2;lokke2++){
      for(lokke=0;lokke<VSTL_MAX(effect->numInputs,effect->numOutputs);lokke++){
	free(vstl->RTbuffer[lokke2][lokke]);
      }
      free(vstl->RTbuffer[lokke2]);
    }
  }

  VSTL_stopParameterthread(vstl);
  VSTLIB_delete(effect);
  free(vstl->ports);
  free(vstl->ports_compaire);

  free(vstl);
}



static void VSTL_connect_port(
			      LADSPA_Handle Instance,
			      unsigned long Port,
			      LADSPA_Data * DataLocation
			      )
{
  struct VSTLadspa *vstl=(struct VSTLadspa *)Instance;
  AEffect *effect=vstl->effect;
  vstl->ports[Port]=DataLocation;
  if(Port>=effect->numInputs+effect->numOutputs){
    pthread_mutex_lock(&vstl->stopstartparameterthreadmutex);
    vstl->ports_compaire[Port]=*(vstl->ports[Port]);
    effect->setParameter(
			 effect,
			 Port - (effect->numInputs+effect->numOutputs),
			 *DataLocation
			 );
    pthread_mutex_unlock(&vstl->stopstartparameterthreadmutex);
  }
}






void _init() {
  int lokke2;
  int numberofplugins;
  struct AEffect **effects=VSTLIB_newCacheList(&numberofplugins);

  for(lokke2=0;lokke2<numberofplugins;lokke2++){
    int lokke;
    LADSPA_Descriptor *vd;
    LADSPA_PortDescriptor *portdescriptors;
    char **portnames;

    char temp[500];
    struct AEffect *effect;

    effect=effects[lokke2];

    vd=VSTDescriptors[lokke2]=calloc(1,sizeof(LADSPA_Descriptor));

    vd->UniqueID=100000+lokke2;
    sprintf(temp,"vst_%s",VSTLIB_getName(effect));
    vd->Label=strdup(temp);
    

    // No, use sockets.
    //    vd->Properties=LADSPA_PROPERTY_HARD_RT_CAPABLE;

    if(getenv("LADSPAVST_RT")!=NULL){
      vd->Properties=LADSPA_PROPERTY_HARD_RT_CAPABLE;
    }

    sprintf(temp,"VST plugin %s available thru vstserver.",VSTLIB_getName(effect));
    vd->Name=strdup(temp);

    vd->Maker=strdup("<Maker information not available>");
    vd->Copyright=strdup("<Copyright information not available>");

    vd->PortCount = effect->numInputs + effect->numOutputs + effect->numParams + 1;

    portnames=calloc(vd->PortCount,sizeof(char*));
    vd->PortRangeHints=calloc(vd->PortCount,sizeof(LADSPA_PortRangeHint));

    portdescriptors=malloc(vd->PortCount * sizeof(LADSPA_PortDescriptor));

    for(lokke=0;lokke<effect->numInputs;lokke++){
      char temp[500];
      portdescriptors[lokke]=(const LADSPA_PortDescriptor)(LADSPA_PORT_INPUT|LADSPA_PORT_AUDIO);
      sprintf(temp,"Audio In %d",lokke);
      portnames[lokke]=strdup(temp);
    }
    for(lokke=effect->numInputs;lokke<effect->numInputs+effect->numOutputs;lokke++){
      portdescriptors[lokke]=LADSPA_PORT_OUTPUT|LADSPA_PORT_AUDIO;
      sprintf(temp,"Audio out %d",lokke-(int)effect->numInputs);
      portnames[lokke]=strdup(temp);
    }
    for(lokke=effect->numInputs+effect->numOutputs;lokke<vd->PortCount - 1;lokke++){
      float val;
      char temp[500];

      portdescriptors[lokke]=LADSPA_PORT_INPUT|LADSPA_PORT_CONTROL;
      portnames[lokke]=malloc(10);

      sprintf(temp,"_paramname_%d",lokke);
      effect->dispatcher(
			 effect,
			 effGetParamName,
			 lokke - (effect->numInputs+effect->numOutputs), 0, portnames[lokke], 0
			 );

      vd->PortRangeHints[lokke].HintDescriptor=LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE;
      vd->PortRangeHints[lokke].LowerBound=0.0f;
      vd->PortRangeHints[lokke].UpperBound=1.0f;

      //      vd->PortRangeHints[lokke].HintDescriptor |= LADSPA_HINT_DEFAULT_MASK;

      sprintf(temp,"_paramval_%d",lokke);

      val=effect->getParameter(effect,lokke - (effect->numInputs+effect->numOutputs));

      if(val<0.4f){
	if(val<0.1f){
	  if(val==0.0f){
	    vd->PortRangeHints[lokke].HintDescriptor |= LADSPA_HINT_DEFAULT_0;
	  }else{
	    vd->PortRangeHints[lokke].HintDescriptor |= LADSPA_HINT_DEFAULT_MINIMUM;
	  }
	}else{
	  vd->PortRangeHints[lokke].HintDescriptor |= LADSPA_HINT_DEFAULT_LOW;
	}
      }else{
	if(val<0.6f){
	  vd->PortRangeHints[lokke].HintDescriptor |= LADSPA_HINT_DEFAULT_MIDDLE;
	}else{
	  if(val>0.9f){
	    if(val==1.0f){
	      vd->PortRangeHints[lokke].HintDescriptor |= LADSPA_HINT_DEFAULT_1;
	    }else{
	      vd->PortRangeHints[lokke].HintDescriptor |= LADSPA_HINT_DEFAULT_MAXIMUM;
	    }
	  }else{
	    vd->PortRangeHints[lokke].HintDescriptor |= LADSPA_HINT_DEFAULT_HIGH;
	  }
	}
      }


    }
    portdescriptors[lokke]=LADSPA_PORT_INPUT|LADSPA_PORT_CONTROL;
    vd->PortRangeHints[lokke].HintDescriptor=LADSPA_HINT_TOGGLED | LADSPA_HINT_DEFAULT_0;
    portnames[lokke]=strdup("Gui_on_off");
    vd->PortRangeHints[lokke].LowerBound=0.0f;
    vd->PortRangeHints[lokke].UpperBound=1.0f;

    vd->PortDescriptors=portdescriptors;
    vd->PortNames=portnames;


    vd->ImplementationData=NULL;
    vd->instantiate=VSTL_instantiate;
    vd->connect_port=VSTL_connect_port;

    if(getenv("LADSPAVST_RT")!=NULL && !strcmp("1",getenv("LADSPAVST_RT"))){
      vd->run=VSTL_RTrun;
    }else{
      vd->run=VSTL_run;
    }

    vd->cleanup=VSTL_cleanup;
  }

  num_plugins=numberofplugins;
  VSTLIB_deleteCacheList(effects);

}

void _fini() {
  return;
}
