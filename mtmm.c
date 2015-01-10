#include "mtmm.h"

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <math.h>

#define DEBUGPRINT 1

#if DEBUGPRINT
#define DBGPRINTF printf
#else
#define DBGPRINTF
#endif

#define NUMOFHEAPS 2

#define NUMOFSIZECLASSES 16
#define SIZECLASSBASE 2

float f = 0.75f;
float K = 0;

struct sSuperblock;
struct sHeap;

typedef struct sBlockHeader
{
   /*
	* @breaf - size of allocated memory as available for user
	*/
   unsigned int mSize;

   struct sSuperblock* superblockBelongs;


} BlockHeader, *pBlockHeader;

typedef struct sBlockHeaderNode
{
	//BlockHeader* block;
	struct sBlockHeaderNode* next;

	unsigned int chunkSize;

} BlockHeaderNode, *pBlockHeaderNode;

typedef struct sSuperblock
{
	BlockHeaderNode* freeList;
	unsigned int using;

	struct sHeap* heapBelongs;

	struct sSuperblock* next;

} Superblock, *pSuperblock;


typedef struct sSizeClass
{
  unsigned int size;

  Superblock* superblocks;
  unsigned int numOfSuperBlocks;

} SizeClass, *pSizeClass;


typedef struct sHeap
{
  SizeClass sizeClass[NUMOFSIZECLASSES]; // for size x

  unsigned int mCPUID; // to hold the CPU ID

  unsigned int using;
  unsigned int all;

  pthread_mutex_t mutex;

} Heap, *pHeap;

typedef struct sHoard
{
  Heap globalHeap;
  Heap heaps[NUMOFHEAPS];
} Hoard;

Hoard hoard; // global Hoard data structure
int hoardIsInitialized = 0;
pHeap globalHeap = &hoard.globalHeap;

void initHoard() {

	if (hoardIsInitialized == 1) return;

	int i, j, r;

	hoard.globalHeap.all = 0;
	hoard.globalHeap.using = 0;

	r = pthread_mutex_init(&hoard.globalHeap.mutex, NULL);
	if (r) {
		perror(NULL);
		return;
	}
	for (j = 0; j < NUMOFSIZECLASSES; ++j) {
		hoard.globalHeap.sizeClass[j].numOfSuperBlocks = 0;
		hoard.globalHeap.sizeClass[j].size = pow(SIZECLASSBASE, j);
		hoard.globalHeap.sizeClass[j].superblocks = NULL;
	}

	for (i = 0; i < NUMOFHEAPS; ++i) {
		hoard.heaps[i].all = 0;
		hoard.heaps[i].using = 0;

		r = pthread_mutex_init(&hoard.heaps[i].mutex, NULL);
		if (r) {
			perror(NULL);
			return;
		}
		for (j = 0; j < NUMOFSIZECLASSES; ++j) {
			hoard.heaps[i].sizeClass[j].numOfSuperBlocks = 0;
			hoard.heaps[i].sizeClass[j].size = pow(SIZECLASSBASE, j);
			hoard.heaps[i].sizeClass[j].superblocks = NULL;
		}
	}

	hoardIsInitialized = 1;
}


SizeClass* getSizeClass(pHeap currentHeap, int currentSizeClass) {

	return &currentHeap->sizeClass[currentSizeClass];
}

void getCurrentSizeClass(int sz, int* currnetSizeClass, int* currentSizeClassPadded) {

	int i;

	// maybe we can optimize this, not now
	for(i = 0; i < NUMOFSIZECLASSES; ++i) {
		int nextStage = pow(SIZECLASSBASE, i);
		if (sz < nextStage) {
			*currnetSizeClass = i;
			*currentSizeClassPadded = nextStage;
			return;
		}
	}

	// if we didnt find big enough, we return the maximum

  *currnetSizeClass = NUMOFSIZECLASSES;
  *currentSizeClassPadded = pow(SIZECLASSBASE, NUMOFSIZECLASSES);

}

/* hash function for getting the heap for the specific thread */
/* can be conflicts, not the best hash algorithm ever */
int getCurrentHeapI() {

  pthread_t self = pthread_self();

  return (self % NUMOFHEAPS);
}

Superblock* getMostFullnessSuperblock(pSizeClass sc, unsigned int sizeNeedToAllocate) {
	//int currentMaxFullnessValue = 0;
	pSuperblock currentMaxFullnessSuperblock = NULL;
	pSuperblock currentSuperblock;
	int i;

	if (sc->numOfSuperBlocks == 0) {
		return NULL;
	}

	currentSuperblock = sc->superblocks;
	if (SUPERBLOCK_SIZE - currentSuperblock->using >= sizeNeedToAllocate) return currentSuperblock;

	for (i = 1; i < sc->numOfSuperBlocks; ++i) {
		currentSuperblock = currentSuperblock->next;

		/* first check that the superblock have space for the new block */
		if (SUPERBLOCK_SIZE - currentSuperblock->using >= sizeNeedToAllocate) {

			return currentSuperblock;
			/* then check who is the maximum */
			//if (currentSuperblock->using > currentMaxFullnessValue) {
			//	currentMaxFullnessValue = currentSuperblock->using;
			//	currentMaxFullnessSuperblock = currentSuperblock;
			//}
		}
	}

	return currentMaxFullnessSuperblock;
}

pBlockHeader allocateFromSuperblock(pSuperblock superblock, unsigned int sizeAllocation) {

	pBlockHeader newBlock = (pBlockHeader)(superblock->freeList + sizeof(BlockHeaderNode));
	newBlock->mSize = sizeAllocation;
	newBlock->superblockBelongs = superblock;


	/* now update the freeList */

	if (superblock->freeList->chunkSize - sizeAllocation >= sizeAllocation) {
		int oldChunkSize = superblock->freeList->chunkSize;
		superblock->freeList = (pBlockHeaderNode)(superblock->freeList + sizeof(BlockHeaderNode) + sizeAllocation);
		superblock->freeList->chunkSize = oldChunkSize - sizeAllocation;
	} else {
		superblock->freeList = superblock->freeList->next;
	}

	superblock->using += sizeAllocation;

	return newBlock;
}

pSuperblock getFreeSuperblockFromGlobal(pSizeClass scGlobal) {
	if (scGlobal->numOfSuperBlocks > 0) {

		scGlobal->superblocks = scGlobal->superblocks->next; // this is for removing the superblock from global, for not mistaken using it twice
		scGlobal->numOfSuperBlocks--;

		globalHeap->all -= SUPERBLOCK_SIZE;
		globalHeap->using -= scGlobal->superblocks->using;

		return scGlobal->superblocks;
	}

	return NULL;
}

pSuperblock createNewSuperblock(pHeap heapBelongs) {

	void* p = mmap(
			NULL,
			SUPERBLOCK_SIZE + sizeof(Superblock), // request the size of superblock and header
			PROT_READ|PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
			-1,
			0);

	if (p == MAP_FAILED) {
		perror(NULL);
		return 0;
	}

	pSuperblock newSuperblock = (pSuperblock)p;
	newSuperblock->using = 0;
	newSuperblock->heapBelongs = heapBelongs;
	newSuperblock->freeList = (pBlockHeaderNode)(p + sizeof(Superblock));
	newSuperblock->freeList->chunkSize = SUPERBLOCK_SIZE;
	newSuperblock->freeList->next = NULL;

	return newSuperblock;
}

/* add the superblock to the heap */
void transferSuperblock(pHeap toHeap, pSuperblock superblockToMove, int sizeClass) {

	// lock heap toHeap
	pthread_mutex_lock(&toHeap->mutex);

	toHeap->sizeClass[sizeClass].numOfSuperBlocks++;

	// add to link list
	superblockToMove->next = toHeap->sizeClass[sizeClass].superblocks;
	toHeap->sizeClass[sizeClass].superblocks = superblockToMove;

	toHeap->all += SUPERBLOCK_SIZE;
	toHeap->using += superblockToMove->using;

	// unlock heap toHeap
	pthread_mutex_unlock(&toHeap->mutex);
}

/* this function removes superblock from the heap and return it, it gives the minimum allocated superblock */
pSuperblock getTheMostEmptySuperblock(pHeap fromHeap, unsigned int sizeClassNum) {

	int currentMinFullnessValue;
	pSuperblock currentMinFullnessSuperblock;
	pSuperblock previousMinFullnessSuperblock;
	pSuperblock currentSuperblock;
	pSuperblock previousSuperblock;
	int i;

	pSizeClass sizeClass = &fromHeap->sizeClass[sizeClassNum];

	if (sizeClass->numOfSuperBlocks == 0) {
		return NULL;
	}

	currentMinFullnessValue = sizeClass->superblocks->using;
	currentMinFullnessSuperblock = sizeClass->superblocks;

	currentSuperblock = sizeClass->superblocks;
	previousSuperblock = NULL;

	for (i = 1; i < sizeClass->numOfSuperBlocks; ++i) {
		previousSuperblock = currentSuperblock;
		currentSuperblock = currentSuperblock->next;

		/* check who is the minimum */
		if (currentSuperblock->using < currentMinFullnessValue) {
			currentMinFullnessValue = currentSuperblock->using;
			currentMinFullnessSuperblock = currentSuperblock;
			previousMinFullnessSuperblock = previousSuperblock;
		}
	}

	fromHeap->all -= SUPERBLOCK_SIZE;
	fromHeap->using -= currentMinFullnessSuperblock->using;

	/* remove superblock from the linked list */
	if (previousMinFullnessSuperblock == NULL) {
		/* we have only one superblock, remove it from heap */
		sizeClass->numOfSuperBlocks = 0;
		sizeClass->superblocks = NULL;
	} else {
		/* remove the pointer from previous node */
		sizeClass->numOfSuperBlocks--;
		previousMinFullnessSuperblock->next = currentMinFullnessSuperblock->next;
	}

	return currentMinFullnessSuperblock;
}

void * malloc2 (size_t sz)
{
	DBGPRINTF("start mymalloc\n");

	initHoard();

	DBGPRINTF("got size for allocation - %d\n", (unsigned int)sz);

	if (sz > SUPERBLOCK_SIZE / 2) {

		DBGPRINTF("found size bigger than half superblock - %d\n", (unsigned int)sz);

	  	int fd;
	  	void *p;



	  	fd = open("/dev/zero", O_RDWR);
	  	if (fd == -1){
	  		perror(NULL);
	  		return 0;
	  	}
	  	p = mmap(0, sz + sizeof(BlockHeader), PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	  	close(fd);

	  	if (p == MAP_FAILED){
	  		perror(NULL);
	  		return 0;
	  	}

	  	((BlockHeader *) p) -> mSize = sz;

	  	DBGPRINTF("created big mmap for the request\n");
	  	return (p + sizeof(BlockHeader));
	}

	DBGPRINTF("small size - allocate in superblock\n");

	DBGPRINTF("getting heap\n");

	int currentHeapI = getCurrentHeapI();
	pHeap currentHeap = &hoard.heaps[currentHeapI];

	DBGPRINTF("found heap %d\n", currentHeapI);

	DBGPRINTF("lock heap %d\n", currentHeapI);

	// lock heap i
	pthread_mutex_lock(&currentHeap->mutex);

	DBGPRINTF("heap %d locked\n", currentHeapI);


	int currentSizeClass;
	int currentSizeClassPadded;

	// add to the size the user wanted the size of the block header and teh size of blocker header node
	sz += sizeof(BlockHeaderNode) + sizeof(BlockHeader);

	getCurrentSizeClass(sz, &currentSizeClass, &currentSizeClassPadded);

	DBGPRINTF("current size class %d - size padded %d\n", currentSizeClass, currentSizeClassPadded);

	SizeClass* sc = getSizeClass(currentHeap, currentSizeClass);

	Superblock* superblock = getMostFullnessSuperblock(sc, currentSizeClassPadded);

	if (superblock == NULL) {
		/* there is no superblock at all, we need to get one */

		/* check if we have available from global */

		// lock global heap
		pthread_mutex_lock(&globalHeap->mutex);

		char unlockedGlobalHeap = 0; // boolean for know if we unlocked the global heap


		SizeClass* scGlobal = getSizeClass(globalHeap, currentSizeClass);
		pSuperblock sbForTransfer = getFreeSuperblockFromGlobal(scGlobal);

		if (sbForTransfer != NULL) {
			globalHeap->using -= sbForTransfer->using;
			currentHeap->using += sbForTransfer->using;

			globalHeap->all -= SUPERBLOCK_SIZE;
			currentHeap->all += SUPERBLOCK_SIZE;

			/* add the superblock to the current heap */
			sbForTransfer->next = currentHeap->sizeClass[currentSizeClass].superblocks;
			currentHeap->sizeClass[currentSizeClass].superblocks = sbForTransfer;

			currentHeap->sizeClass[currentSizeClass].numOfSuperBlocks++;

			superblock = sbForTransfer;


		} else {
			/* if not, create one */

			// unlock global heap
			// do it now for saving time before allocating memory non related to the global heap
			pthread_mutex_unlock(&globalHeap->mutex);
			unlockedGlobalHeap = 1;


			pSuperblock newSuperblock = createNewSuperblock(currentHeap);

			// insert the new superblock to the head of the linked list of superblocks
			newSuperblock->next = currentHeap->sizeClass[currentSizeClass].superblocks;
			currentHeap->sizeClass[currentSizeClass].superblocks = newSuperblock;


			currentHeap->all += SUPERBLOCK_SIZE;
			currentHeap->sizeClass[currentSizeClass].numOfSuperBlocks++;

			superblock = newSuperblock;

		}

		// this check is for not unlock the global heap twice
		// we do this for keep the fast unlock of the global heap as we can
		if (unlockedGlobalHeap == 0) {
			// unlock global heap
			pthread_mutex_unlock(&globalHeap->mutex);
		}
	}


	/* allocate from the superblock */
	pBlockHeader block = allocateFromSuperblock(superblock, currentSizeClassPadded);

	currentHeap->using += currentSizeClassPadded;

	// unlock heap i
	pthread_mutex_unlock(&currentHeap->mutex);

	DBGPRINTF("end mymalloc\n");

	return (block + sizeof(BlockHeader));
}



void free2 (void * ptr)
{
	if (ptr == NULL) return;

	DBGPRINTF("start myfree\n");

	pBlockHeader blockHeader = (pBlockHeader)(ptr - sizeof(BlockHeader));

	unsigned int sizeToRemove = blockHeader->mSize;

	if (sizeToRemove > SUPERBLOCK_SIZE / 2)
	{
		unsigned int size = blockHeader->mSize; // the size is the overall size, include the BlockHeader size
		if (munmap(ptr - sizeof(BlockHeader), size) < 0)
		{
		     perror(NULL);
		     return;
		}

	}

	pSuperblock superblockBelongs = blockHeader->superblockBelongs;
	pHeap heapBelongs = superblockBelongs->heapBelongs;

	// lock the heap
	pthread_mutex_lock(&heapBelongs->mutex);

	/* dealock from superblock */
	pBlockHeaderNode blockHeaderNode = (pBlockHeaderNode)(blockHeader - sizeof(BlockHeaderNode));
	blockHeaderNode->chunkSize += sizeToRemove;

	// return the node to the free list
	blockHeaderNode->next = superblockBelongs->freeList;
	superblockBelongs->freeList = blockHeaderNode;

	/* remove using from superblock and heap */
	heapBelongs->using -= sizeToRemove;
	superblockBelongs->using -= sizeToRemove;

	/* check if we dealing with the global heap */
	if (heapBelongs == globalHeap) {
		// unlock the heap (current == global)
		pthread_mutex_unlock(&heapBelongs->mutex);

		return;
	}

	/* we are not in the global */
	/* check if the heap is almost empty */

	if ((heapBelongs->using < heapBelongs->all - K * SUPERBLOCK_SIZE) &&
			(heapBelongs->using < (1 - f) * heapBelongs->all)) {

		int currentSizeClass;
		int currentSizeClassPadded;

		getCurrentSizeClass(sizeToRemove, &currentSizeClass, &currentSizeClassPadded);

		pSuperblock superblockToMove = getTheMostEmptySuperblock(heapBelongs, currentSizeClass);
		transferSuperblock(globalHeap, superblockToMove, currentSizeClass);
	}

	// unlock the heap
	pthread_mutex_unlock(&heapBelongs->mutex);

	DBGPRINTF("done myfree\n");
	
}

void * realloc2 (void * ptr, size_t sz)
{


	DBGPRINTF("myrealloc\n");

	return malloc2(sz);
}




