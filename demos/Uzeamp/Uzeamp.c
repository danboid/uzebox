/*
 *  Uzebox(tm) Uzeamp
 *  Copyright (C) 2009-2024 Uze
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  Uzebox is a reserved trade mark
 *
 *  ---
 *
 *  Uzeamp is a music player that supports standard WAVE files encoded in 8-bit mono with 15734Hz sampling rate.
 *
 *  Features:
 *  - Plays WAV files, 8-bit unsigned mono file at 15734Hz (the NTSC line rate).
 *  - Supports WAV file metadata extension for song name and artist.
 *  - Supports FAT16/FAT32, SD and SDHC, file fragmentation
 *  - Supports very old and slow SD cards that were shipped in the Uzebox kits
 *  - Support skins/color schemes
 *  - VU meter
 *  - Auto-play next song
 *
 *  Compatible WAVE files can be made using FFMPEG. This will produce a file in the correct format and striping
 *  all metadata from the orginal file while adding custom song title and artist name.
 *
 *  ffmpeg -i input.mp3 -f wav -bitexact -acodec pcm_u8 -ac 1 -ar 15734 -map_metadata -1 -metadata title="<song title>" -metadata artist="<song artist>"  output.wav
 *
 *  See the wiki page for details: https://uzebox.org/wiki/Uzeamp
*/

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>
#include <uzebox.h>
#include <petitfatfs/pff.h>
#include <petitfatfs/diskio.h>
#include "data/tiles.inc"

#define BFONT 	0
#define SFONT	1
#define BLANK 	0
#define OK		0
#define MAX_DISP_FILES 	11
#define MAX_FILES 		128
#define CURSOR_INIT_X	3
#define CURSOR_INIT_Y	11
#define VU_METER_Y_OFFSET 5

#define SCROLL_NONE	0
#define SCROLL_UP	1
#define SCROLL_DOWN	2

#define WAVE_PCM 1

#define PLAYER_STOPPED 			0x00
#define PLAYER_STARTED 			0x01
#define PLAYER_PAUSED  			0x02
#define PLAYER_SONG_ENDED  		0x03
#define PLAYER_LOADING  		0x04

#define ERR_INVALID_FORMAT		0x01
#define ERR_INVALID_STRUCTURE	0x02
#define ERR_NOT_FOUND			0x03

/**
 * Struct representing the wave file header
 * see: https://www.robotplanet.dk/audio/wav_meta_data/
 * 		http://soundfile.sapp.org/doc/WaveFormat/
 */
typedef struct{
	//header
	unsigned char chunkID[4];
	unsigned long chunkSize;
	unsigned char format[4];

	//fmt chunk
	unsigned char subchunk1ID[4];
	unsigned long subchunk1Size;
	unsigned int audioFormat;
	unsigned int numChannels;
	unsigned long sampleRate;
	unsigned long byteRate;
	unsigned int blockAlign;
	unsigned int bitsPerSample;

	//data chunk
	unsigned char subchunk2ID[4];
	unsigned long subchunk2Size;

} RIFFheader;

typedef struct{
	//data chunk
	unsigned char subchunkID[4];
	unsigned long subchunkSize;
	unsigned char buffer[];
} RIFFDataChunk;

/**
 * Union struct to save space
 */
typedef union {
	unsigned char buffer[512];
	RIFFheader riffHeader;
	RIFFDataChunk dataChunk;
} SdSector;

/**
 * Wave files information read from the directory
 */
typedef struct {
	char filename[9];				//fat16 filename minus extension
	unsigned long fileSize;
} SdFile;

typedef struct {
	char filename[13];
	char info[129];					//Buffer for the artist name and display info used in the scroller
	unsigned long currentSector;
	unsigned long sectorsCount;
	unsigned long position;
	unsigned long riffDataOffset;
} Song;


void animateTextLine(char *infoSongStr, bool reset);
void animateTextLine_P(const char *infoSongStr, bool reset);
int findTag(char *tag, char *dest,unsigned int destLenght,unsigned char dataOffset);
u8  getChar(int index);
void getFilePlayingTime(unsigned long songSize,char *buffer);
u8 loadWaveInfoBlock(SdFile *file);
void initSdCard();
void drawDigits(unsigned long currentSectorNo );
void PrintByte2(u8 x,u8 y,u8 val,bool zeropad,u8 maxDigits, u8 font);
void PrintChar2(u8 x,u8 y,char ch,u8 font);
void PrintString2(u8 x, u8 y, char *str, u8 font);
void PrintString2_P(u8 x, u8 y, const char *str, u8 font);
void processPlayer();
void sdError(u8 code,u8 res);
void setString(char *str,char c,u8 maxLen);
void startSong(long SectorNo,bool loadInfoBlock);
void updateVuMeter();
void updateProgressBar(unsigned long currentSector);
void updateFilesList(u8 scrollType);
bool process(bool buttonPressed,u8 buttonID,bool repeatOn);


//PetitFS vars
FATFS fs;			// Work area (file system object) for the volume
FILINFO fno;		//File object
DIR dir;			// Directory object
WORD br;			// File read count
FRESULT fres;		// Petit FatFs function common result code

//Global variables
SdSector sector;				//buffer to hold an SD sector and RIFF header
SdFile files[MAX_FILES];		//infos about the files found on the card
u8 fileCount=0;					//number of files loaded in files[]
u16 playingFileIndex=0;			//file in the files[] array currently selected
u8  playerStatus=PLAYER_STOPPED;//current status of the player
s16 vuFrameMaxSample=0;			//loudest sample played in the last vsync (vu meter)
u8 cursorSongIndex=0;			//Song index at the cursor
Song currentSong;				//Variables for the current song

/**
 * User callback invoqued on each VSYNC (60 fps)
 */
void VsyncCallBack(void){
	processPlayer();
	if(playerStatus ==  PLAYER_LOADING) animateTextLine_P(NULL,false);
}

/**
 * Used for debugging and printing with printf().
 * Escape characters are suported:
 * \n - Line feed
 * \r - Back to beginning of the same line
 * \f - Back to top of the screen
 */
int putchar_printf(char c, FILE *stream) {
	static u8 x=0,y=0;
	if(c=='\n'){
		x=1;y++;
	}else if(c=='\r'){
		x=1;
	}else if(c=='\f'){
		x=1;y=1;
	}else if(c>=' ' && c <= '~'){
		PrintChar2(x++,y,c,BFONT);
	}
	return 0;
}
static FILE stream = FDEV_SETUP_STREAM(putchar_printf, NULL, _FDEV_SETUP_WRITE);


int main(){
	//Skins data. Uzed to mask bits in the video port's data direction register hence altering the displayed colors.
	u8 currentSkin=0,cursorDelay,cursorState,cursorX,cursorY,cursorPrevY;
	u8 skins[]={0xff,0xfc,0xe7,0xbe,0xbd,0x77,0x3e,0x27};
	u16 buttons;
	bool cueing;
	long cueSector;

	stdout=&stream;
	SetTileTable(main_tileset);
	//SetSpritesTileTable(main_tileset);
	SetUserPreVsyncCallback(&VsyncCallBack);
	//Draw the overall GUI
	DrawMap(0,0,map_main);
	//Draw the vu meter
	DrawMap(19,VU_METER_Y_OFFSET-1,map_overlaVuMeter);
	for(int i=0;i<8;i++)DrawMap2(19+i,VU_METER_Y_OFFSET,map_vuMeterBarOff); //vu meter
	//Draw the wave file parameters
	DrawMap(19,VU_METER_Y_OFFSET+1,map_overlayParams);
	//draw progress bar
	SetTile(2,8,128);SetTile(27,8,138);//beginning+end
	for(u8 i=0;i<24;i++)SetTile(i+3,8,129);

	drawDigits(0);

	initSdCard();

	PrintString2_P(10,15,PSTR("LOADING..."),BFONT);

	//Open the SD card's root directory and search for up to MAX_FILES wave files
	fileCount=0;
    fres = pf_opendir(&dir, "/");

    if (fres == FR_OK) {
        while (1) {
            fres = pf_readdir(&dir, &fno);
            if (fres != FR_OK || fno.fname[0] == 0) break;
			char *pos = strstr(fno.fname, ".WAV");
			if(pos != NULL){
				for(u8 i=0;i<8;i++){
					if(fno.fname[i]=='.'){
						files[fileCount].filename[i]=0;
						break;
					}
					files[fileCount].filename[i]=fno.fname[i];
				}
				files[fileCount].fileSize=fno.fsize;
				fileCount++;
				if(fileCount==MAX_FILES) break;
			}
        }
    }else{
    	sdError(0,fres);
    }
    updateFilesList(SCROLL_NONE);


    /**
     * Main loop that operate the GUI
     */
    cueSector=0;
    cueing=false;
    cursorState=1;cursorDelay=0;
    cursorSongIndex=0;
    cursorX=CURSOR_INIT_X;cursorY=cursorPrevY=CURSOR_INIT_Y;

  	while(1)
  	{
		WaitVsync(1);
		buttons=ReadJoypad(0);

		//Start a new song, pause and resume the same song
		if(process(buttons&BTN_START,0,false)){
			if(playerStatus == PLAYER_STARTED && cursorSongIndex==playingFileIndex){
				playerStatus = PLAYER_PAUSED;
			}else if(cursorSongIndex!=playingFileIndex || playerStatus == PLAYER_STOPPED){
				playingFileIndex=cursorSongIndex;
				startSong(0, true);
			}else{
				playerStatus = PLAYER_STARTED;
			}
		}

		//Move cursor up
		if(process(buttons&BTN_UP,1,true)){
			if(cursorSongIndex>0){
				if(cursorY>CURSOR_INIT_Y && cursorSongIndex<(MAX_DISP_FILES))cursorY--;
				cursorSongIndex--;
				cursorState=1;
				cursorDelay=0;
				updateFilesList(SCROLL_DOWN);
			}
		}

		//Move cursor down
		if(process(buttons&BTN_DOWN,2,true)){

			if(cursorSongIndex<(fileCount-1)){
				if(cursorSongIndex<(MAX_DISP_FILES-1))cursorY++;
				cursorSongIndex++;
				cursorState=1;
				cursorDelay=0;
				updateFilesList(SCROLL_UP);
			}
		}

		//Cycle through the various color schemes
		if(process(buttons&BTN_SELECT,3,false)){
			currentSkin++;
			if(currentSkin==sizeof(skins))currentSkin=0;
			DDRC=skins[currentSkin];
		}

		//Fast forward the song. Pressing A button at the same time will cue faster
		if(buttons&BTN_SR){
			if(playerStatus == PLAYER_STARTED){
				if(!cueing){
					cueSector=currentSong.currentSector;
				}
				cueSector+=50;//fast-forward
				if(buttons&BTN_A)cueSector+=400;//fast-forwards even faster

				if(cueSector>=(currentSong.sectorsCount-20)){
					cueSector=currentSong.sectorsCount-20;
				}
				cueing=true;
			}
		}

		//Fast backward the song. Pressing A button at the same time will cue faster
		if(buttons&BTN_SL){
			if(playerStatus == PLAYER_STARTED){
				if(!cueing){
					cueSector=currentSong.currentSector;
				}
				cueSector-=100; //fast-backward
				if(buttons&BTN_A)cueSector-=400;//fast-backwards even faster
				if(cueSector<0){
					cueSector=0;
				}
				cueing=true;
			}
		}

		//Check if there is more songs to play
		if(playerStatus == PLAYER_SONG_ENDED){
			if(cursorSongIndex<(fileCount-1)){
				if(cursorSongIndex<(MAX_DISP_FILES-1))cursorY++;
				cursorSongIndex++;
				playingFileIndex=cursorSongIndex;
				updateFilesList(SCROLL_UP);
				startSong(0, true);
			}else{
				playerStatus = PLAYER_STOPPED;
			}
		}

		//When releasing the fast-forward/fast-backward button
		//stop the song then restart it at the new position
		if( (buttons&BTN_SL)==0 && (buttons&BTN_SR)==0 && cueing==true){
			cueing=false;
			startSong(cueSector,false);
		}

		//update digits and cue slider
		if(cueing){
			drawDigits(cueSector);
			updateProgressBar(cueSector);
		}else if(playerStatus == PLAYER_STARTED || playerStatus == PLAYER_STOPPED){
			drawDigits(currentSong.currentSector);
			updateProgressBar(currentSong.currentSector);
		}

		//update the cursor
		PrintChar2(cursorX-1,cursorPrevY,' ',BFONT); //erase previous cursor location
		cursorPrevY=cursorY;
		if(playerStatus != PLAYER_STARTED)cursorState=1;
		if(cursorState == 1){
			PrintChar2(cursorX-1,cursorY,'^',BFONT);
		}else{
			PrintChar2(cursorX-1,cursorY,' ',BFONT);
		}
		cursorDelay++;
		if(cursorDelay>20){
			cursorDelay=0;
			cursorState^=1;
		}

		updateVuMeter();
		animateTextLine(NULL,false);
   }

}
/**
 * Handle auto-repeat
 */
bool process(bool buttonPressed,u8 buttonID,bool repeatOn){
	static u8 autoRepeatDelay[4],autoRepeatRate[4];


	if(!(buttonPressed)){
		autoRepeatRate[buttonID]=20;
		autoRepeatDelay[buttonID]=0;
		return false;
	}

	if(buttonPressed && autoRepeatDelay[buttonID]==0){
		autoRepeatDelay[buttonID]=autoRepeatRate[buttonID];
		return true;
	}
	if(autoRepeatDelay[buttonID]>0 && repeatOn){
		autoRepeatDelay[buttonID]--;
		if(autoRepeatDelay[buttonID]==0){
			autoRepeatRate[buttonID]=3;
		}
	}
	return false;
}

/**
 * Redraws the whole file list windows accounting for the selected song and scrolling
 */
void updateFilesList(u8 scrollType){
	char timebuf[9];
	u8 disp=0;
	u8 byteDigits;

	//calculate how much space we need to allocate for the file number
	if(fileCount<10){
		byteDigits=1;
	}else if(fileCount<100){
		byteDigits=2;
	}else{
		byteDigits=3;
	}

	if(cursorSongIndex>=MAX_DISP_FILES && scrollType==SCROLL_UP){
		disp=cursorSongIndex-MAX_DISP_FILES+1;

		//scroll up content of window
		for(u8 y=11;y<11+MAX_DISP_FILES-1;y++){
			for(u8 x=CURSOR_INIT_X;x<27;x++){
				vram[(VRAM_TILES_H*y)+x]=vram[(VRAM_TILES_H*(y+1))+x];
			}
		}
		//add new file at bottom
		for(u8 j=1;j<16;j++) PrintChar2(CURSOR_INIT_X+j,MAX_DISP_FILES+10,'_',BFONT); //erase line
		PrintByte2(CURSOR_INIT_X+byteDigits-1,MAX_DISP_FILES+10,MAX_DISP_FILES+disp,true,byteDigits,BFONT);
		PrintChar2(CURSOR_INIT_X+byteDigits,MAX_DISP_FILES+10,'.',BFONT);
		PrintString2(CURSOR_INIT_X+byteDigits+1,MAX_DISP_FILES+10,files[MAX_DISP_FILES+disp-1].filename,BFONT);
		setString(timebuf,'_',8);	//init time string buffer;
		getFilePlayingTime(files[MAX_DISP_FILES+disp-1].fileSize,timebuf);
		PrintString2(CURSOR_INIT_X+16,MAX_DISP_FILES+10,timebuf,BFONT);

	}else if(cursorSongIndex>=MAX_DISP_FILES-1 && scrollType==SCROLL_DOWN){
		disp=cursorSongIndex-MAX_DISP_FILES+1;

		//scroll down content of window
		for(u8 y=9+MAX_DISP_FILES;y>9;y--){
			for(u8 x=CURSOR_INIT_X;x<27;x++){
				vram[(VRAM_TILES_H*(y+1))+x]=vram[(VRAM_TILES_H*y)+x];
			}
		}
		//add new file at top
		for(u8 j=1;j<16;j++) PrintChar2(CURSOR_INIT_X+j,11,'_',BFONT); 			//erase line
		PrintByte2(CURSOR_INIT_X+byteDigits-1,11,disp+1,true,byteDigits,BFONT);	//file number
		PrintChar2(CURSOR_INIT_X+byteDigits,11,'.',BFONT);						//separator
		PrintString2(CURSOR_INIT_X+byteDigits+1,11,files[disp].filename,BFONT);	//filename
		setString(timebuf,'_',8);												//init time string buffer;
		getFilePlayingTime(files[disp].fileSize,timebuf);						//compute song lenght
		PrintString2(CURSOR_INIT_X+16,11,timebuf,BFONT);						//print song lenght

	}else if(scrollType==SCROLL_NONE){
		for(u8 i=0;(i<MAX_DISP_FILES && i<fileCount);i++){
			for(u8 j=1;j<16;j++) PrintChar2(CURSOR_INIT_X+j,i+11,'_',BFONT); //erase line
			PrintByte2(CURSOR_INIT_X+byteDigits-1,i+11,i+disp+1,true,byteDigits,BFONT);
			PrintChar2(CURSOR_INIT_X+byteDigits,i+11,'.',BFONT);
			PrintString2(CURSOR_INIT_X+byteDigits+1,i+11,files[i+disp].filename,BFONT);
			setString(timebuf,'_',8);												//init time string buffer;
			getFilePlayingTime(files[i+disp].fileSize,timebuf);						//compute song lenght
			PrintString2(CURSOR_INIT_X+16,i+11,timebuf,BFONT);						//print song lenght
		}
	}

}

/**
 * Draws the progress bar
 */
void updateProgressBar(unsigned long currentSector){
	if(currentSong.sectorsCount==0 && currentSector==0) return;

	SetTile(3,8,128); //beginning
	SetTile(26,8,138); //end

	//bar is 24 tiles wide * 8 = 192 pixels
	u8 pixels=192*currentSector/currentSong.sectorsCount;
	u8 fullTiles=pixels/8;
	u8 tilePixels=pixels%8;

	for(u8 i=0;i<24;i++){
		if(i<fullTiles){
			SetTile(i+3,8,137);
		}else if(tilePixels>0){
			SetTile(i+3,8,129+tilePixels);
			tilePixels=0;
		}else{
			SetTile(i+3,8,129);
		}
	}
}

/**
 * Display a simple vu meter.
 */
#define VU_METER_DECAY_RATE 1
#define VU_METER_WAIT_FRAME	3
void updateVuMeter(){
	const u8 thresholds[8] = {2, 11, 26, 47, 77, 118, 175, 250};
	static u8 wait=0,current_segments=0,max=0;

	u8 *buf=mix_buf;
	if(mix_bank==1) buf+=MIX_BANK_SIZE;

	//get the loudest sample for the bank
	u8 sample=0;
	for(int i=0;i<MIX_BANK_SIZE;i++){
		sample=(abs((s8)(buf[i]+0x80))*2);
		if(sample>max)max=sample;
	}

	if(wait==VU_METER_WAIT_FRAME){
		//check how many thresholds the audio value exceeds
		u8 segments = 0;
		for (int i = 0; i < 8; i++) {
			if (max >= thresholds[i]) {
				segments++;
			} else {
				break; //stop if audio value is below the current threshold
			}
		}

	   //if the new target is higher, update immediately
		if (segments > current_segments) {
			current_segments = segments;
		} else if (current_segments > 0) {
			//otherwise, apply decay
			current_segments = current_segments > VU_METER_DECAY_RATE ? current_segments - VU_METER_DECAY_RATE : 0;
		}

		for (int i = 0; i < 8; i++) {
			if (i < current_segments) {
				DrawMap2(19+i,VU_METER_Y_OFFSET,map_vuMeterBarOn);
			} else {
				DrawMap2(19+i,VU_METER_Y_OFFSET,map_vuMeterBarOff);
			}
		}
		wait=0;
		max=0;
	}else{
		wait++;
	}
}

/**
 * Called once per vsync in order to load the audio ring buffer
 * with new data from the SD card.
 */
void processPlayer()
{
	u8 *buf=mix_buf;
	u16 bytesRead=0;

	if(mix_bank==1) buf+=MIX_BANK_SIZE;

	if(playerStatus == PLAYER_STARTED){
		fres=pf_read(buf,MIX_BANK_SIZE,&bytesRead);
		if(fres!=FR_OK) sdError(1,fres);
		if(bytesRead < MIX_BANK_SIZE || currentSong.currentSector == currentSong.sectorsCount){
			//clear the ring buffer to avoid clicks
			for(int i=0;i<MIX_BUF_SIZE;i++)	mix_buf[i] = 0x80;
			playerStatus = PLAYER_SONG_ENDED;
		}
		currentSong.position += MIX_BANK_SIZE;
		currentSong.currentSector = currentSong.position / 512;

	}else{
		//if not playing, clear half-buffer
		for(int i=0;i<MIX_BANK_SIZE;i++){
			*buf++ = 0x80;
		}
	}

}

/**
 * Start playing a song.
 *
 * Open the files, loads the song's meta for the scroller then start the player.
 */
void startSong(long firstSector, bool loadInfoBlock){
	//first insure the vsync player is stopped
	playerStatus =  PLAYER_LOADING;

	currentSong.currentSector=firstSector;
	currentSong.position=firstSector * 512;

	//display a loading message for large files (pf_lseek is very slow on large files)
	animateTextLine_P(PSTR("... LOADING ... LOADING ... LOADING"),true);

	strcpy(currentSong.filename,files[playingFileIndex].filename);
	strcat(currentSong.filename,".WAV");

	fres=pf_open(currentSong.filename);
	if(fres!=FR_OK) sdError(2,fres);

	if(loadInfoBlock){
		u8 res=loadWaveInfoBlock(&files[playingFileIndex]);
		if(res==ERR_INVALID_FORMAT){
			animateTextLine_P(PSTR("UNSUPPORTED WAVE FILE FORMAT...MUST BE 15734Hz, 8BITS, MONO. SEE UZEBOX.ORG/WIKI/UZEAMP..."),true);
			return;
		}else if(res==ERR_INVALID_STRUCTURE){
			animateTextLine_P(PSTR("UNSUPPORTED WAVE FILE STRUCTURE..."),true);
			return;
		}
	}

	//Advance read pointer beginning of audio data
	fres=pf_lseek((firstSector*512)+currentSong.riffDataOffset);
	if(fres!=FR_OK) sdError(3,fres);

	animateTextLine(currentSong.info,true);

	//Activate vsync handler to process audio audio
	playerStatus = PLAYER_STARTED;
}


/**
 * Search wave file meta data for the artist name and display information.
 * These are stored under the IART and DISP chunks.
 * see: https://www.robotplanet.dk/audio/wav_meta_data/
 * 		http://soundfile.sapp.org/doc/WaveFormat/
 *
 * Limitations:
 *- required meta data must fit with a single sector (512 bytes).
 *- metadata must be either right after data chunk (usually put there by most tools)
 *  or right after fmt chunk (like ffmpeg)
 *- keep metadata to just title and artist to avoid issues (other be ignored anyway)
 */
u8 debug=0;
u8 loadWaveInfoBlock(SdFile *file){
	unsigned long infoBlockStartAddr;
	u8 i,pos=0,c;
	u16 bytesRead=0;
	char infoTemp[64];

	//Read the first sector to get the Riff header
	fres=pf_read(sector.buffer, 512,&bytesRead);
	if(fres!=FR_OK) sdError(4,fres);

	//check if its a valid WAV file
	if(	sector.riffHeader.format[0]=='W' &&
		sector.riffHeader.format[1]=='A' &&
		sector.riffHeader.format[2]=='V' &&
		sector.riffHeader.format[3]=='E'){

		//next chunk is always "fmt ", check if wave is correct format
		//Support 15750hz used by old .wav files as well as the correct 15734hz
		if(sector.riffHeader.audioFormat!=WAVE_PCM || sector.riffHeader.numChannels!=1
				|| !(sector.riffHeader.sampleRate ==15734 || sector.riffHeader.sampleRate==15750)
				|| sector.riffHeader.bitsPerSample!=8){
			return ERR_INVALID_FORMAT;
		}

		//check if the next chunk is sound data ou metadata
		if(	sector.riffHeader.subchunk2ID[0]=='d' &&
			sector.riffHeader.subchunk2ID[1]=='a' &&
			sector.riffHeader.subchunk2ID[2]=='t' &&
			sector.riffHeader.subchunk2ID[3]=='a'){

			//compute the address of the meta data that resides after the sound data
			//Starts with the chunk ID "INFO"
			infoBlockStartAddr=sector.riffHeader.subchunk2Size + sizeof(sector.riffHeader) + 1;
			currentSong.sectorsCount=(sector.riffHeader.subchunk2Size/512);
			currentSong.riffDataOffset=(sizeof(sector.riffHeader) + 1);

			//return if no INFO chunk after audio data
			if(infoBlockStartAddr>=sector.riffHeader.chunkSize){
				strcpy_P(currentSong.info,PSTR("UNTITLED"));
				return OK;
			}

			//advance read pointer to meta data start
			fres=pf_lseek(infoBlockStartAddr);
			if(fres!=FR_OK) sdError(5,fres);

			//read the meta data
			fres=pf_read(sector.buffer, 512,&bytesRead);
			if(fres!=FR_OK) sdError(6,fres);

		}else if(sector.riffHeader.subchunk2ID[0]=='L' &&
				 sector.riffHeader.subchunk2ID[1]=='I' &&
				 sector.riffHeader.subchunk2ID[2]=='S' &&
				 sector.riffHeader.subchunk2ID[3]=='T'){

			currentSong.sectorsCount=(sector.riffHeader.chunkSize-sector.riffHeader.subchunk1Size-sector.riffHeader.subchunk2Size)/512;
				currentSong.riffDataOffset=36+18+sector.riffHeader.subchunk2Size;
		}else{
			//unrecognized structure
			return ERR_INVALID_STRUCTURE;
		}

		//file->currentSector=0;
		currentSong.currentSector=0;

		if(findTag("INFO",NULL,0,0)==0){

			//printf("here");while(1);


			//find the artist name metadata
			if(findTag("IART",infoTemp,sizeof(infoTemp),4)==OK){
				//append to the concatenated artist name
				pos=0;
				if(infoTemp[0]!=0){
					i=0;
					while(i<sizeof(infoTemp)){
						c=infoTemp[i++];
						if(c==0) break;
						currentSong.info[pos++]=c;
					}
					if(pos!=0){
						currentSong.info[pos++]=' ';
						currentSong.info[pos++]='-';
						currentSong.info[pos++]=' ';
					}
				}
			}

			//search for the song name
			if(findTag("INAM",infoTemp,sizeof(infoTemp),4)==OK){
				if(infoTemp[0]!=0){
					i=0;
					while(i<sizeof(infoTemp)){
						c=infoTemp[i++];
						if(c==0) break;
						currentSong.info[pos++]=c;
					}
				}
			}

			//return if no INFO chunk after audio data
			if(pos==0){
				strcpy_P(currentSong.info,PSTR("UNTITLED"));
			}else{
				currentSong.info[pos]=0;
			}
			return OK;
		}else{
			currentSong.info[0]=0;
			return ERR_INVALID_STRUCTURE;
		}
	}

	return ERR_INVALID_FORMAT;
}

/**
 * Scan the SD buffer for the specified chunk name (tag) and return the value
 */
int findTag(char *tag, char *dest,unsigned int destLenght,unsigned char dataOffset){
	unsigned long i=0,size;
	int j;

	while(1){
		if( getChar(i)==tag[0] && getChar(i+1)==tag[1] && getChar(i+2)==tag[2] && getChar(i+3)==tag[3]){
			if(dest!=NULL){
				i+=4;
				//get info chunk size
				size=getChar(i)+(getChar(i+1)<<8);//+(getChar(i+2)<<8)+(getChar(i+3)<<8);
				if(size>=(destLenght-1))size=(destLenght-1);
				i+=dataOffset;

				for(j=0;j<size;j++){
					dest[j]=getChar(i+j);
				}
				dest[j]=0;
			}
			return OK; //found!
		}

		i++;
		if(i>=512){
			dest[0]=0;
			return ERR_NOT_FOUND; //not found!
		}
	}

}

unsigned char getChar(int index){
	u8 c=sector.buffer[index];
	if(c>=0x7d) c='?'; //clip unsupported characters above ascii 127
	if(c>=97) c-=32; //convert lowercase to uppercase
	return c;
}

/**
 * Draw the song's timestamp digits
 */
void drawDigits(unsigned long currentSectorNo ){
	unsigned long hours,minutes,seconds,temp;
	unsigned char digit1, digit2,x=3;
	//15734 bytes/sec @ 512bytes/sector
	//31 sectors/sec
	//1844 sectors/min
	//110630 sectors/hour

	hours=currentSectorNo/110630;
	temp=(currentSectorNo-(hours*110630));
	minutes=temp/1844;
	seconds=(temp-(minutes*1844))/31;

	//print hours
	digit1=(hours/10)%10;
	digit2=hours%10;
	DrawMap2(x+0,4,map_digit0+(digit1*8));
	DrawMap2(x+2,4,map_digit0+(digit2*8));
	DrawMap2(x+4,4,map_digitSep);
	digit1=(minutes/10)%10;
	digit2=minutes%10;
	DrawMap2(x+5,4,map_digit0+(digit1*8));
	DrawMap2(x+7,4,map_digit0+(digit2*8));
	DrawMap2(x+9,4,map_digitSep);
	digit1=(seconds/10)%10;
	digit2=seconds%10;
	DrawMap2(x+10,4,map_digit0+(digit1*8));
	DrawMap2(x+12,4,map_digit0+(digit2*8));

}

void getFilePlayingTime(unsigned long songSize,char *buffer){
	unsigned long hours,minutes,seconds,temp;
	unsigned char digit1, digit2;

	//convert to sectors
	songSize /= 512;

	//15734 bytes/sec @ 512bytes/sector
	//31 sectors/sec
	//1844 sectors/min
	//110630 sectors/hour

	hours=songSize/110630;
	temp=(songSize-(hours*110630));
	minutes=temp/1844;
	seconds=(temp-(minutes*1844))/31;

	//print hours
	digit1=(hours/10)%10;
	digit2=hours%10;
	if(hours>0 && digit1>0) buffer[0]=digit1+'0';
	if(hours>0) buffer[1]=digit2+'0';
	if(hours>0) buffer[2]=':';

	digit1=(minutes/10)%10;
	digit2=minutes%10;
	if(hours>0 || digit1>0)buffer[3]=digit1+'0';
	if(hours>0 || minutes>0)buffer[4]=digit2+'0';
	buffer[5]=':';

	digit1=(seconds/10)%10;
	digit2=seconds%10;
	buffer[6]=digit1+'0';
	buffer[7]=digit2+'0';

	buffer[8]=0;
}

/**
 * Scrolls the song's information
 */
void animateTextLine(char *infoSongStr, bool reset){
	static char *infoSongStrCopy;
	static unsigned char pos=0,wait=0;
	unsigned char curPos=pos,c;

	if(infoSongStr==NULL){
		//animate previous setted string
		infoSongStr=infoSongStrCopy;
	}else{
		infoSongStrCopy=infoSongStr;
	}

	size_t songLen=strlen(infoSongStr);

	if(reset){
		pos=0;
		curPos=0;
		if(songLen<=24){
			for(char i=0;i<24;i++)PrintChar2(3+i,7,' ',SFONT);
			PrintString2(3,7,infoSongStr,SFONT);
		}
		wait=20;
	}

	if(wait>=20){
		if(songLen>24){
			for(int i=0;i<24;i++){
				if(curPos>=songLen){
					c=32;
				}else{
					c=infoSongStr[curPos];
				}
				PrintChar2(i+3,7,c,SFONT);
				curPos++;
				if(curPos>=(songLen+4)) curPos=0;
			}

			wait=0;
			pos++;
			if(pos>=(songLen+4)) pos=0;
		}
	}

	wait++;

}
/**
 * Scrolls information in flash
 */
void animateTextLine_P(const char *infoSongStr, bool reset){
	static const char *infoSongStrCopy;
	static unsigned char pos=0,wait=0;
	unsigned char curPos=pos,c;

	if(infoSongStr==NULL){
		//animate previous setted string
		infoSongStr=infoSongStrCopy;
	}else{
		infoSongStrCopy=infoSongStr;
	}

	size_t songLen=strlen_P(infoSongStr);

	if(reset){
		pos=0;
		curPos=0;
		if(songLen<=24){
			for(char i=0;i<24;i++)PrintChar2(3+i,7,' ',SFONT);
			PrintString2_P(3,7,infoSongStr,SFONT);
		}
		wait=20;
	}

	if(wait>=20){
		if(songLen>24){
			for(int i=0;i<24;i++){
				if(curPos>=songLen){
					c=32;
				}else{
					c=pgm_read_byte(&(infoSongStr[curPos]));
				}
				PrintChar2(i+3,7,c,SFONT);
				curPos++;
				if(curPos>=(songLen+4)) curPos=0;
			}

			wait=0;
			pos++;
			if(pos>=(songLen+4)) pos=0;
		}
	}

	wait++;

}
/**
 * Custom function that prints a string in RAM and supports 2 fonts.
 * Each must be 32 chars long and located at the beginning of the tile set.
 * See tiles.png
 */
void PrintString2(u8 x, u8 y, char *str, u8 font){
	char c;
	while((c=*str++)){
		if(c==126)c=92; //fix tilde character for long filename because it is out of the font
		PrintChar2(x++,y,c,font);
	}
}

/**
 * Custom function that prints a single character and supports 2 fonts.
 * See PrintString2 for restrictions.
 */
void PrintChar2(u8 x,u8 y,char c,u8 font){
	if(c>='a' && c<='z')c&=0b11011111;	//convert to uppercase
	if(font) c+=64; //address second font
	SetTile(x,y,c-32);
}

void PrintByte2(u8 x,u8 y,u8 val,bool zeropad,u8 maxDigits, u8 font){
	unsigned char c,i;

	for(i=0;i<maxDigits;i++){
		c=val%10;
		if(val>0 || i==0){
			SetTile(x--,y,c+'0'-32+(font?64:0));
		}else{
			if(zeropad){
				SetTile(x--,y,c+'0'-32+(font?64:0));
			}else{
				SetFont(x--,y,' '-32+(font?64:0));
			}
		}
		val=val/10;
	}
}

/**
 * Custom function that prints a string in FLASH and supports 2 fonts.
 * See PrintString2 for restrictions.
 */
void PrintString2_P(u8 x, u8 y, const char *str, u8 font){
	int i=0;
	char c;
	while(1){
		c=pgm_read_byte(&(str[i++]));
		if(c==0)break;
		PrintChar2(x++,y,c,font);
	}
}

/**
 * Fills the specified string with the specified character upton maxLen.
 */
void setString(char *str,char c,u8 maxLen){
	for(u8 i=0;i<maxLen;i++)str[i]=c;
}


/**
 * Initialize the SD card, polling up to 5 seconds
 * and displays an error message if unsuccessful
 */
void initSdCard(){
	//try 10 times to initialize then fail
	for(u8 i=0;i<10;i++){
		fres=pf_mount(&fs);
		if(fres==FR_OK){
			Fill(3,11,24,11,BLANK);
			return;
		}

		PrintString2_P(6,15,PSTR("INITIALIZING SD..."),BFONT);

		//deassert sd card and wait
		PORTD &= ~(1<<6);
		WaitVsync(30);
	}

	PrintString2_P(5,15,PSTR("NO SD CARD DETECTED!"),BFONT);
	while(1);
}

/**
 * Stop the program and display an SD card error message.
 * locationCode is a code that indicate where the error occured in order
 * to help debugging.
 */
void sdError(u8 locationCode,u8 pffCode){
	playerStatus = PLAYER_STOPPED;
	PORTD &= ~(1<<6); //deassert sd card

	Fill(2,11,26,11,BLANK);
//	PrintString2_P(7,14,PSTR("SD CARD ERROR!"),BFONT);
//	PrintString2_P(8,16,PSTR("PFF CODE:"),BFONT);
//	PrintString2_P(8,17,PSTR("LOC CODE:"),BFONT);
//	PrintByte(19,16,pffCode,true);
//	PrintByte(19,17,locationCode,true);

	initSdCard();
	wdt_enable(WDTO_15MS);		//Enable the watchdog timer to reset the console
	while(1);
}
