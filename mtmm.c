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

	int i, j;

	hoard.globalHeap.all = 0;
	hoard.globalHeap.using = 0;
	for (j = 0; j < NUMOFSIZECLASSES; ++j) {
		hoard.globalHeap.sizeClass[j].numOfSuperBlocks = 0;
		hoard.globalHeap.sizeClass[j].size = pow(SIZECLASSBASE, j);
		hoard.globalHeap.sizeClass[j].superblocks = NULL;
	}

	for (i = 0; i < NUMOFHEAPS; ++i) {
		hoard.heaps[i].all = 0;
		hoard.heaps[i].using = 0;
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
	int currentMaxFullnessValue = 0;
	pSuperblock currentMaxFullnessSuperblock = NULL;
	int i;

	if (sc->numOfSuperBlocks == 0) {
		return NULL;
	}

	for (i = 0; i < sc->numOfSuperBlocks; ++i) {
		pSuperblock currentSuperblock = &sc->superblocks[i];

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

pSuperblock getFreeSuperblock(pSizeClass scGlobal) {
	if (scGlobal->numOfSuperBlocks > 0) {

		scGlobal->superblocks = scGlobal->superblocks->next;
		scGlobal->numOfSuperBlocks -= 1;

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

		char unlockedGlobalHeap = 0; // boolean for know if we unlocked the global heap


		SizeClass* scGlobal = getSizeClass(globalHeap, currentSizeClass);
		pSuperblock sbForTransfer = getFreeSuperblock(scGlobal);

		if (sbForTransfer != NULL) {
			globalHeap->using -= sbForTransfer->using;
			currentHeap->using += sbForTransfer->using;

			globalHeap->all -= SUPERBLOCK_SIZE;
			currentHeap->all += SUPERBLOCK_SIZE;


		} else {
			/* if not, create one */

			pSuperblock newSuperblock = createNewSuperblock(currentHeap);

			currentHeap->all += SUPERBLOCK_SIZE;
			currentHeap->sizeClass[currentSizeClass].numOfSuperBlocks++;

			// insert the new superblock to the head of the linked list of superblocks
			newSuperblock->next = currentHeap->sizeClass[currentSizeClass].superblocks;
			currentHeap->sizeClass[currentSizeClass].superblocks = newSuperblock;

			superblock = newSuperblock;

			// unlock global heap
			unlockedGlobalHeap = 1;

		}

		// this check is for not unlock the global heap twice
		// we do this for keep the fast unlock of the global heap as we can
		if (unlockedGlobalHeap == 0) {
			// unlock global heap
		}
	}


	/* allocate from the superblock */
	pBlockHeader block = allocateFromSuperblock(superblock, currentSizeClassPadded);

	currentHeap->using += currentSizeClassPadded;

	// unlock heap i

	DBGPRINTF("end mymalloc\n");

	return (block + sizeof(BlockHeader));
}



void free2 (void * ptr)
{
	if (ptr == NULL) return;

	DBGPRINTF("start myfree\n");

	pBlockHeader blockHeader = (pBlockHeader)(ptr - sizeof(BlockHeader));

	if (blockHeader->mSize > SUPERBLOCK_SIZE / 2)
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



	DBGPRINTF("done myfree\n");
	
}

void * realloc2 (void * ptr, size_t sz)
{


	DBGPRINTF("myrealloc\n");

	return malloc2(sz);
}




