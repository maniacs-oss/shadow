/*
 * The Shadow Simulator
 * Copyright (c) 2010-2011, Rob Jansen
 * See LICENSE for licensing information
 */

#include "shadow.h"

struct _Topology {
	/* the file path of the graphml file */
	GString* graphPath;

	/******/
	/* START global igraph lock - igraph is not thread-safe!*/
	GMutex graphLock;

	/* the imported igraph graph data - operations on it after initializations
	 * MUST be locked because igraph is not thread-safe! */
	igraph_t graph;

	/* the edge weights currently used when computing shortest paths.
	 * this is protected by the graph lock */
	igraph_vector_t* edgeWeights;

	/* keep track of how long we spend computing shortest paths,
	 * protected by the graph lock */
	gdouble shortestPathTotalTime;

	/* END global igraph lock - igraph is not thread-safe!*/
	/******/

	/* each connected virtual host is assigned to a PoI vertex. we store the mapping to the
	 * vertex index so we can correctly lookup the assigned edge when computing latency.
	 * virtualIP->vertexIndex (stored as pointer) */
	GHashTable* virtualIP;
	GRWLock virtualIPLock;

	/* cached latencies to avoid excessive shortest path lookups
	 * store a cache table for every connected address
	 * fromAddress->toAddress->Path* */
	GHashTable* pathCache;
	GRWLock pathCacheLock;

	/* graph properties of the imported graph */
	igraph_integer_t clusterCount;
	igraph_integer_t vertexCount;
	igraph_integer_t edgeCount;
	igraph_bool_t isConnected;

	MAGIC_DECLARE;
};

typedef struct _ConnectAssist ConnectAssist;
struct _ConnectAssist {
	GQueue* candidates;
	gchar* typeHint;
	gchar* geocodeHint;
	gchar* ipHint;
};

typedef void (*EdgeNotifyFunc)(Topology* top, igraph_integer_t edgeIndex, gpointer userData);
typedef void (*VertexNotifyFunc)(Topology* top, igraph_integer_t vertexIndex, gpointer userData);

static gboolean _topology_loadGraph(Topology* top) {
	MAGIC_ASSERT(top);
	/* initialize the built-in C attribute handler */
	igraph_attribute_table_t* oldHandler = igraph_i_set_attribute_table(&igraph_cattribute_table);

	/* get the file */
	FILE* graphFile = fopen(top->graphPath->str, "r");
	if(!graphFile) {
		critical("fopen returned NULL, problem opening graph file path '%s'", top->graphPath->str);
		return FALSE;
	}

	info("reading graphml topology graph at '%s'", top->graphPath->str);

	gint result = igraph_read_graph_graphml(&top->graph, graphFile, 0);
	fclose(graphFile);
	if(result != IGRAPH_SUCCESS) {
		critical("igraph_read_graph_graphml return non-success code %i", result);
		return FALSE;
	}

	info("successfully read graphml topology graph at '%s'", top->graphPath->str);

	return TRUE;
}

static gboolean _topology_checkGraphProperties(Topology* top) {
	MAGIC_ASSERT(top);
	gint result = 0;

	info("checking graph properties...");

	/* IGRAPH_WEAK means the undirected version of the graph is connected
	 * IGRAPH_STRONG means a vertex can reach all others via a directed path
	 * we must be able to send packets in both directions, so we want IGRAPH_STRONG */
	result = igraph_is_connected(&top->graph, &(top->isConnected), IGRAPH_STRONG);
	if(result != IGRAPH_SUCCESS) {
		critical("igraph_is_connected return non-success code %i", result);
		return FALSE;
	}

	igraph_integer_t clusterCount;
	result = igraph_clusters(&top->graph, NULL, NULL, &(top->clusterCount), IGRAPH_STRONG);
	if(result != IGRAPH_SUCCESS) {
		critical("igraph_clusters return non-success code %i", result);
		return FALSE;
	}

	/* it must be connected */
	if(!top->isConnected || top->clusterCount > 1) {
		critical("topology must be but is not strongly connected", top->graphPath->str);
		return FALSE;
	}

	info("graph is %s with %u %s",
			top->isConnected ? "strongly connected" : "disconnected",
			(guint)top->clusterCount, top->clusterCount == 1 ? "cluster" : "clusters");

	info("checking graph attributes...");

	/* now check list of all attributes */
	igraph_strvector_t gnames, vnames, enames;
	igraph_vector_t gtypes, vtypes, etypes;
	igraph_strvector_init(&gnames, 1);
	igraph_vector_init(&gtypes, 1);
	igraph_strvector_init(&vnames, igraph_vcount(&top->graph));
	igraph_vector_init(&vtypes, igraph_vcount(&top->graph));
	igraph_strvector_init(&enames, igraph_ecount(&top->graph));
	igraph_vector_init(&etypes, igraph_ecount(&top->graph));

	result = igraph_cattribute_list(&top->graph, &gnames, &gtypes, &vnames, &vtypes, &enames, &etypes);
	if(result != IGRAPH_SUCCESS) {
		critical("igraph_cattribute_list return non-success code %i", result);
		return FALSE;
	}

	gint i = 0;
	for(i = 0; i < igraph_strvector_size(&gnames); i++) {
		gchar* name = NULL;
		igraph_strvector_get(&gnames, (glong) i, &name);
		debug("found graph attribute '%s'", name);
	}
	for(i = 0; i < igraph_strvector_size(&vnames); i++) {
		gchar* name = NULL;
		igraph_strvector_get(&vnames, (glong) i, &name);
		debug("found vertex attribute '%s'", name);
	}
	for(i = 0; i < igraph_strvector_size(&enames); i++) {
		gchar* name = NULL;
		igraph_strvector_get(&enames, (glong) i, &name);
		debug("found edge attribute '%s'", name);
	}

	info("successfully verified graph attributes");

	return TRUE;
}

static void _topology_checkGraphVerticesHelperHook(Topology* top, igraph_integer_t vertexIndex, gpointer userData) {
	MAGIC_ASSERT(top);

	/* get vertex attributes: S for string and N for numeric */
	const gchar* idStr = VAS(&top->graph, "id", vertexIndex);
	const gchar* typeStr = VAS(&top->graph, "type", vertexIndex);

	if(g_strstr_len(idStr, (gssize)-1, "poi")) {
		const gchar* ipStr = VAS(&top->graph, "ip", vertexIndex);
		const gchar* geocodeStr = VAS(&top->graph, "geocode", vertexIndex);
		igraph_real_t bwup = VAN(&top->graph, "bandwidthup", vertexIndex);
		igraph_real_t bwdown = VAN(&top->graph, "bandwidthdown", vertexIndex);
		igraph_real_t ploss = VAN(&top->graph, "packetloss", vertexIndex);

		debug("found vertex %li (%s), type=%s ip=%s geocode=%s "
				"bandwidthup=%f bandwidthdown=%f packetloss=%f",
				(glong)vertexIndex, idStr, typeStr, geocodeStr, bwup, bwdown, ploss);
	} else {
		debug("found vertex %li (%s), type=%s",
				(glong)vertexIndex, idStr, typeStr);
	}
}

static igraph_integer_t _topology_iterateAllVertices(Topology* top, VertexNotifyFunc hook, gpointer userData) {
	MAGIC_ASSERT(top);
	utility_assert(hook);

	/* we will iterate through the vertices */
	igraph_vit_t vertexIterator;
	gint result = igraph_vit_create(&top->graph, igraph_vss_all(), &vertexIterator);
	if(result != IGRAPH_SUCCESS) {
		critical("igraph_vit_create return non-success code %i", result);
		return -1;
	}

	/* count the vertices as we iterate */
	igraph_integer_t vertexCount = 0;
	while (!IGRAPH_VIT_END(vertexIterator)) {
		long int vertexIndex = IGRAPH_VIT_GET(vertexIterator);

		/* call the hook function for each edge */
		hook(top, (igraph_integer_t) vertexIndex, userData);

		vertexCount++;
		IGRAPH_VIT_NEXT(vertexIterator);
	}

	/* clean up */
	igraph_vit_destroy(&vertexIterator);

	return vertexCount;
}

static gboolean _topology_checkGraphVertices(Topology* top) {
	MAGIC_ASSERT(top);

	info("checking graph vertices...");

	igraph_integer_t vertexCount = _topology_iterateAllVertices(top, _topology_checkGraphVerticesHelperHook, NULL);
	if(vertexCount < 0) {
		/* there was some kind of error */
		return FALSE;
	}

	top->vertexCount = igraph_vcount(&top->graph);
	if(top->vertexCount != vertexCount) {
		warning("igraph_vcount %f does not match iterator count %f", top->vertexCount, vertexCount);
	}

	info("%u graph vertices ok", (guint) top->vertexCount);

	return TRUE;
}

static void _topology_checkGraphEdgesHelperHook(Topology* top, igraph_integer_t edgeIndex, gpointer userData) {
	MAGIC_ASSERT(top);

	igraph_integer_t fromVertexIndex, toVertexIndex;
	gint result = igraph_edge(&top->graph, edgeIndex, &fromVertexIndex, &toVertexIndex);
	if(result != IGRAPH_SUCCESS) {
		critical("igraph_edge return non-success code %i", result);
		return;
	}

	const gchar* fromIDStr = VAS(&top->graph, "id", fromVertexIndex);
	const gchar* toIDStr = VAS(&top->graph, "id", toVertexIndex);

	/* get edge attributes: S for string and N for numeric */
	igraph_real_t latency = EAN(&top->graph, "latency", edgeIndex);
	igraph_real_t jitter = EAN(&top->graph, "jitter", edgeIndex);
	igraph_real_t ploss = EAN(&top->graph, "packetloss", edgeIndex);

	debug("found edge %li from vertex %li (%s) to vertex %li (%s) latency=%s jitter=%s packetloss=%s",
			(glong)edgeIndex, (glong)fromVertexIndex, fromIDStr, (glong)toVertexIndex, toIDStr,
			latency, jitter, ploss);
}

static igraph_integer_t _topology_iterateAllEdges(Topology* top, EdgeNotifyFunc hook, gpointer userData) {
	MAGIC_ASSERT(top);
	utility_assert(hook);

	/* we will iterate through the edges */
	igraph_eit_t edgeIterator;
	gint result = igraph_eit_create(&top->graph, igraph_ess_all(IGRAPH_EDGEORDER_ID), &edgeIterator);
	if(result != IGRAPH_SUCCESS) {
		critical("igraph_eit_create return non-success code %i", result);
		return -1;
	}

	/* count the edges as we iterate */
	igraph_integer_t edgeCount = 0;
	while (!IGRAPH_EIT_END(edgeIterator)) {
		long int edgeIndex = IGRAPH_EIT_GET(edgeIterator);

		/* call the hook function for each edge */
		hook(top, (igraph_integer_t) edgeIndex, userData);

		edgeCount++;
		IGRAPH_EIT_NEXT(edgeIterator);
	}

	igraph_eit_destroy(&edgeIterator);

	return edgeCount;
}

static gboolean _topology_checkGraphEdges(Topology* top) {
	MAGIC_ASSERT(top);

	info("checking graph edges...");

	igraph_integer_t edgeCount = _topology_iterateAllEdges(top, _topology_checkGraphEdgesHelperHook, NULL);
	if(edgeCount < 0) {
		/* there was some kind of error */
		return FALSE;
	}

	top->edgeCount = igraph_ecount(&top->graph);
	if(top->edgeCount != edgeCount) {
		warning("igraph_vcount %f does not match iterator count %f", top->edgeCount, edgeCount);
	}

	info("%u graph edges ok", (guint) top->edgeCount);

	return TRUE;
}

static gboolean _topology_checkGraph(Topology* top) {
	if(!_topology_checkGraphProperties(top) || !_topology_checkGraphVertices(top) ||
			!_topology_checkGraphEdges(top)) {
		return FALSE;
	}

	message("successfully parsed graphml at '%s' and validated topology: "
			"graph is %s with %u %s, %u %s, and %u %s",
			top->graphPath->str, top->isConnected ? "strongly connected" : "disconnected",
			(guint)top->clusterCount, top->clusterCount == 1 ? "cluster" : "clusters",
			(guint)top->vertexCount, top->vertexCount == 1 ? "vertex" : "vertices",
			(guint)top->edgeCount, top->edgeCount == 1 ? "edge" : "edges");

	return TRUE;
}

static gboolean _topology_extractEdgeWeights(Topology* top) {
	MAGIC_ASSERT(top);

	g_mutex_lock(&top->graphLock);

	/* create new or clear existing edge weights */
	if(!top->edgeWeights) {
		top->edgeWeights = g_new0(igraph_vector_t, 1);
	} else {
		igraph_vector_destroy(top->edgeWeights);
		memset(top->edgeWeights, 0, sizeof(igraph_vector_t));
	}

	/* now we have fresh memory */
	gint result = igraph_vector_init(top->edgeWeights, 0);
	if(result != IGRAPH_SUCCESS) {
		g_mutex_unlock(&top->graphLock);
		critical("igraph_vector_init return non-success code %i", result);
		return FALSE;
	}

	/* use the 'latency' edge attribute as the edge weight */
	result = EANV(&top->graph, "latency", top->edgeWeights);
	g_mutex_unlock(&top->graphLock);
	if(result != IGRAPH_SUCCESS) {
		critical("igraph_cattribute_EANV return non-success code %i", result);
		return FALSE;
	}

	return TRUE;
}

static void _topology_clearCache(Topology* top) {
	MAGIC_ASSERT(top);
	g_rw_lock_writer_lock(&(top->pathCacheLock));
	if(top->pathCache) {
		g_hash_table_destroy(top->pathCache);
		top->pathCache = NULL;
	}
	g_rw_lock_writer_unlock(&(top->pathCacheLock));

	message("path cache cleared, spent %f seconds computing shortest paths", top->shortestPathTotalTime);
}

static Path* _topology_getPathFromCache(Topology* top, Address* source, Address* destination) {
	MAGIC_ASSERT(top);

	Path* path = NULL;
	g_rw_lock_reader_lock(&(top->pathCacheLock));

	if(top->pathCache) {
		/* look for the source first level cache */
		ShadowID srcID = address_getID(source);
		gpointer sourceCache = g_hash_table_lookup(top->pathCache, GUINT_TO_POINTER(srcID));

		if(sourceCache) {
			/* check for the path to destination in source cache */
			ShadowID dstID = address_getID(destination);
			path = g_hash_table_lookup(sourceCache, GUINT_TO_POINTER(dstID));
		}
	}

	g_rw_lock_reader_unlock(&(top->pathCacheLock));

	/* NULL if cache miss */
	return path;
}

static void _topology_storePathInCache(Topology* top, Address* source, Address* destination, Path* path) {
	MAGIC_ASSERT(top);

	ShadowID srcID = address_getID(source);
	ShadowID dstID = address_getID(destination);

	g_rw_lock_writer_lock(&(top->pathCacheLock));

	/* create latency cache on the fly */
	if(!top->pathCache) {
		/* stores hash tables for source address caches */
		top->pathCache = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)g_hash_table_destroy);
	}

	GHashTable* sourceCache = g_hash_table_lookup(top->pathCache, GUINT_TO_POINTER(srcID));
	if(!sourceCache) {
		/* dont have a cache for this source yet, create one now */
		sourceCache = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)path_free);
		g_hash_table_replace(top->pathCache, GUINT_TO_POINTER(srcID), sourceCache);
	}

	g_hash_table_replace(sourceCache, GUINT_TO_POINTER(dstID), path);

	g_rw_lock_writer_unlock(&(top->pathCacheLock));
}

static igraph_integer_t _topology_getConnectedVertexIndex(Topology* top, Address* address) {
	MAGIC_ASSERT(top);

	/* find the vertex where this virtual ip was attached */
	gpointer vertexIndexPtr = NULL;
	in_addr_t ip = address_toNetworkIP(address);

	g_rw_lock_reader_lock(&(top->virtualIPLock));
	gboolean found = g_hash_table_lookup_extended(top->virtualIP, GUINT_TO_POINTER(ip), NULL, &vertexIndexPtr);
	g_rw_lock_reader_unlock(&(top->virtualIPLock));

	if(!found) {
		warning("address %s is not connected to the topology", address_toHostIPString(address));
		return (igraph_integer_t) -1;
	}

	return (igraph_integer_t) GPOINTER_TO_INT(vertexIndexPtr);
}

/* !! note: igraph is not thread safe, so graphLock must be held while calling this function */
static Path* _topology_computePath(Topology* top, Address* srcAddress, Address* dstAddress) {
	MAGIC_ASSERT(top);

	igraph_integer_t srcVertexIndex = _topology_getConnectedVertexIndex(top, srcAddress);
	igraph_integer_t dstVertexIndex = _topology_getConnectedVertexIndex(top, dstAddress);
	if(srcVertexIndex < 0 || dstVertexIndex < 0) {
		/* not connected to a vertex */
		return NULL;
	}

	/* setup the destination as a vertex selector with one possible vertex */
	igraph_vs_t dstVertexSet;
	gint result = igraph_vs_1(&dstVertexSet, dstVertexIndex);
	if(result != IGRAPH_SUCCESS) {
		critical("igraph_vs_1 return non-success code %i", result);
		return FALSE;
	}

	/* initialize our result vector where the 1 resulting path will be stored */
	igraph_vector_ptr_t resultPaths;
	result = igraph_vector_ptr_init(&resultPaths, 1);
	if(result != IGRAPH_SUCCESS) {
		critical("igraph_vector_ptr_init return non-success code %i", result);
		return FALSE;
	}

	/* initialize our 1 result element to hold the path vertices */
	igraph_vector_t resultPathVertices;
	result = igraph_vector_init(&resultPathVertices, 0);
	if(result != IGRAPH_SUCCESS) {
		critical("igraph_vector_init return non-success code %i", result);
		return FALSE;
	}

	/* assign our element to the result vector */
	igraph_vector_ptr_set(&resultPaths, 0, &resultPathVertices);
	utility_assert(&resultPathVertices == igraph_vector_ptr_e(&resultPaths, 0));

	g_mutex_lock(&top->graphLock);

	const gchar* srcIDStr = VAS(&top->graph, "id", srcVertexIndex);
	const gchar* dstIDStr = VAS(&top->graph, "id", dstVertexIndex);

	debug("computing shortest path between vertex %li (%s) and vertex %li (%s)",
			(glong)srcVertexIndex, srcIDStr, (glong)dstVertexIndex, dstIDStr);

	/* time the dijkstra algorithm */
	GTimer* pathTimer = g_timer_new();

	/* run dijkstra's shortest path algorithm */
#ifndef IGRAPH_VERSION
	result = igraph_get_shortest_paths_dijkstra(&top->graph, &resultPaths,
			srcVertexIndex, dstVertexSet, top->edgeWeights, IGRAPH_OUT);
#else
	result = igraph_get_shortest_paths_dijkstra(&top->graph, &resultPaths, NULL,
			srcVertexIndex, dstVertexSet, top->edgeWeights, IGRAPH_OUT);
#endif

	/* track the time spent running the algorithm */
	gdouble elapsedSeconds = g_timer_elapsed(pathTimer, NULL);
	g_timer_destroy(pathTimer);
	top->shortestPathTotalTime += elapsedSeconds;

	if(result != IGRAPH_SUCCESS) {
		g_mutex_unlock(&top->graphLock);
		critical("igraph_get_shortest_paths_dijkstra return non-success code %i", result);
		return FALSE;
	}

	/* there are multiple chances to drop a packet here:
	 * psrc : loss rate from source vertex
	 * plink ... : loss rate on the links between source-vertex and destination-vertex
	 * pdst : loss rate from destination vertex
	 *
	 * The reliability is then the combination of the probability
	 * that its not dropped in each case:
	 * P = ((1-psrc)(1-plink)...(1-pdst))
	 */

	/* now get edge latencies and loss rates from the list of vertices (igraph fail!) */
	GString* pathString = g_string_new(NULL);
	igraph_real_t totalLatency = 0, edgeLatency = 0;
	igraph_real_t totalReliability = 1, edgeReliability = 1;
	igraph_integer_t edgeIndex = 0, fromVertexIndex = 0, toVertexIndex = 0;

	/* reliability for the src and dst vertices */
	totalReliability *= (1.0f - VAN(&top->graph, "packetloss", srcVertexIndex));
	totalReliability *= (1.0f - VAN(&top->graph, "packetloss", dstVertexIndex));

	/* now get latency and reliability from each edge in the path */
	glong nVertices = igraph_vector_size(&resultPathVertices);

	/* the first vertex is our starting point
	 * igraph_vector_size can be 0 for paths to ourself */
	if(nVertices > 0) {
		fromVertexIndex = VECTOR(resultPathVertices)[0];
		const gchar* fromIDStr = VAS(&top->graph, "id", fromVertexIndex);
		g_string_append_printf(pathString, "%s", fromIDStr);
	}
	if(nVertices < 2){
		/* we have no edges, src and dst are in the same vertex, or path to self */
		totalLatency = 1.0;
	} else {
		/* iterate the edges in the path and sum the latencies */
		for (gint i = 1; i < nVertices; i++) {
			/* get the edge */
			toVertexIndex = VECTOR(resultPathVertices)[i];
	#ifndef IGRAPH_VERSION
			result = igraph_get_eid(&top->graph, &edgeIndex, fromVertexIndex, toVertexIndex, (igraph_bool_t)TRUE);
	#else
			result = igraph_get_eid(&top->graph, &edgeIndex, fromVertexIndex, toVertexIndex, (igraph_bool_t)TRUE, (igraph_bool_t)TRUE);
	#endif
			if(result != IGRAPH_SUCCESS) {
				g_mutex_unlock(&top->graphLock);
				warning("igraph_get_eid return non-success code %i", result);
				return FALSE;
			}

			//edgeLatency = VECTOR(*(top->currentEdgeWeights))[(gint)edgeIndex];
			edgeLatency = EAN(&top->graph, "latency", edgeIndex);
			totalLatency += edgeLatency;

			edgeReliability = 1.0f - EAN(&top->graph, "packetloss", edgeIndex);
			totalReliability *= edgeReliability;

			/* accumulate path information */
			const gchar* toIDStr = VAS(&top->graph, "id", toVertexIndex);
			g_string_append_printf(pathString, "--[%f,%f]-->%s", edgeLatency, edgeReliability, toIDStr);

			/* update for next edge */
			fromVertexIndex = toVertexIndex;
		}
	}

	g_mutex_unlock(&top->graphLock);

	debug("shortest path %s-->%s is %f ms with %f loss, path: %s", srcIDStr, dstIDStr,
			totalLatency, 1-totalReliability, pathString->str);

	/* clean up */
	g_string_free(pathString, TRUE);
	igraph_vector_clear(&resultPathVertices);
	igraph_vector_ptr_destroy(&resultPaths);

	/* success */
	return path_new((gdouble) totalLatency, (gdouble) totalReliability);
}

static gboolean _topology_getPathEntry(Topology* top, Address* srcAddress, Address* dstAddress,
		gdouble* latency, gdouble* reliability) {
	MAGIC_ASSERT(top);

	/* check for a cache hit */
	Path* path = _topology_getPathFromCache(top, srcAddress, dstAddress);
	if(!path) {
		/* cache miss, compute the path using shortest latency path from src to dst */
		path = _topology_computePath(top, srcAddress, dstAddress);
		if(path) {
			/* cache the latency and reliability we just computed */
			_topology_storePathInCache(top, srcAddress, dstAddress, path);
		}
	}

	if(path) {
		if(latency) {
			*latency = path_getLatency(path);
		}
		if(reliability) {
			*reliability = path_getReliability(path);
		}
		return TRUE;
	} else {
		/* some error computing or caching path */
		return FALSE;
	}
}

gdouble topology_getLatency(Topology* top, Address* srcAddress, Address* dstAddress) {
	MAGIC_ASSERT(top);
	gdouble latency = 0;
	if(_topology_getPathEntry(top, srcAddress, dstAddress, &latency, NULL)) {
		return latency;
	} else {
		return (gdouble) -1;
	}
}

gdouble topology_getReliability(Topology* top, Address* srcAddress, Address* dstAddress) {
	MAGIC_ASSERT(top);
	gdouble reliability = 0;
	if(_topology_getPathEntry(top, srcAddress, dstAddress, NULL, &reliability)) {
		return reliability;
	} else {
		return (gdouble) -1;
	}
}

gboolean topology_isRoutable(Topology* top, Address* srcAddress, Address* dstAddress) {
	MAGIC_ASSERT(top);
	return topology_getLatency(top, srcAddress, dstAddress) > -1;
}

static in_addr_t _topology_getLongestPrefixMatch(GList* ipSet, in_addr_t ip) {
	utility_assert(ipSet);

	in_addr_t bestMatch = 0;
	in_addr_t bestIP = 0;

	GList* nextIPItem = ipSet;
	while(nextIPItem) {
		in_addr_t nextIP = (in_addr_t) GPOINTER_TO_UINT(nextIPItem->data);
		in_addr_t match = (nextIP & ip);
		if(match > bestMatch) {
			bestMatch = match;
			bestIP = nextIP;
		}
		nextIPItem = nextIPItem->next;
	}

	return bestIP;
}

static void _topology_connectHelperHook(Topology* top, igraph_integer_t vertexIndex, ConnectAssist* assist) {
	MAGIC_ASSERT(top);
	utility_assert(assist);

	/* make sure we hold the graph lock when iterating with this helper! */

	const gchar* idStr = VAS(&top->graph, "id", vertexIndex);

	if(g_strstr_len(idStr, (gssize)-1, "poi")) {
		const gchar* typeStr = VAS(&top->graph, "type", vertexIndex);
		const gchar* ipStr = VAS(&top->graph, "ip", vertexIndex);
		const gchar* geocodeStr = VAS(&top->graph, "geocode", vertexIndex);

		/* XXX FIXME fix this to properly filter! */
		g_queue_push_tail(assist->candidates, GINT_TO_POINTER(vertexIndex));
	}
}

void topology_connect(Topology* top, Address* address, Random* randomSourcePool,
		gchar* ipHint, gchar* clusterHint, gchar* typeHint, guint64* bwDownOut, guint64* bwUpOut) {
	MAGIC_ASSERT(top);
	utility_assert(address);

	in_addr_t nodeIP = address_toNetworkIP(address);
	igraph_integer_t vertexIndex = (igraph_integer_t) 0;
	igraph_integer_t* vertexIndexPtr = NULL;

	ConnectAssist* assist = g_new0(ConnectAssist, 1);
	assist->candidates = g_queue_new();
	assist->geocodeHint = clusterHint;
	assist->ipHint = ipHint;
	assist->typeHint = typeHint;

	g_mutex_lock(&top->graphLock);
	_topology_iterateAllVertices(top, (VertexNotifyFunc) _topology_connectHelperHook, assist);
	g_mutex_unlock(&top->graphLock);

	guint numCandidates = g_queue_get_length(assist->candidates);
	utility_assert(numCandidates > 0);

	/* if more than one candidate, grab a random one */
	if(numCandidates > 1) {
		gdouble randomDouble = random_nextDouble(randomSourcePool);
		guint count = (guint) round((gdouble)(numCandidates * randomDouble));
		while(count > 0) {
			vertexIndexPtr = g_queue_pop_head(assist->candidates);
			count--;
		}
	} else {
		vertexIndexPtr = g_queue_pop_head(assist->candidates);
	}

	g_queue_free(assist->candidates);
	g_free(assist);
	assist = NULL;

	/* make sure the vertex is legitimate */
	utility_assert(vertexIndexPtr);
	vertexIndex = *vertexIndexPtr;
	utility_assert(vertexIndex > (igraph_integer_t) -1);

	/* attach it, i.e. store the mapping so we can route later */
	g_rw_lock_writer_lock(&(top->virtualIPLock));
	g_hash_table_replace(top->virtualIP, GUINT_TO_POINTER(nodeIP), GINT_TO_POINTER(vertexIndex));
	g_rw_lock_writer_unlock(&(top->virtualIPLock));

	g_mutex_lock(&top->graphLock);

	/* give them the default cluster bandwidths if they asked */
	if(bwUpOut) {
		*bwUpOut = (guint64) VAN(&top->graph, "bandwidthup", vertexIndex);
	}
	if(bwDownOut) {
		*bwDownOut = (guint64) VAN(&top->graph, "bandwidthdown", vertexIndex);
	}

	const gchar* idStr = VAS(&top->graph, "id", vertexIndex);
	const gchar* typeStr = VAS(&top->graph, "type", vertexIndex);
	const gchar* ipStr = VAS(&top->graph, "ip", vertexIndex);
	const gchar* geocodeStr = VAS(&top->graph, "geocode", vertexIndex);

	g_mutex_unlock(&top->graphLock);

	info("connected address '%s' to point of interest '%s' (%s, %s, %s)",
			address_toHostIPString(address), idStr, ipStr, geocodeStr, typeStr);
}

void topology_disconnect(Topology* top, Address* address) {
	MAGIC_ASSERT(top);
	in_addr_t ip = address_toNetworkIP(address);

	g_rw_lock_writer_lock(&(top->virtualIPLock));
	g_hash_table_remove(top->virtualIP, GUINT_TO_POINTER(ip));
	g_rw_lock_writer_unlock(&(top->virtualIPLock));
}

void topology_free(Topology* top) {
	MAGIC_ASSERT(top);

	/* clear the virtual ip table */
	g_rw_lock_writer_lock(&(top->virtualIPLock));
	if(top->virtualIP) {
		g_hash_table_destroy(top->virtualIP);
		top->virtualIP = NULL;
	}
	g_rw_lock_writer_unlock(&(top->virtualIPLock));
	g_rw_lock_clear(&(top->virtualIPLock));

	/* this functions grabs and releases the pathCache write lock */
	_topology_clearCache(top);
	g_rw_lock_clear(&(top->pathCacheLock));

	/* clear the graph */
	g_mutex_lock(&top->graphLock);

	if(top->graphPath) {
		g_string_free(top->graphPath, TRUE);
	}

	if(top->edgeWeights) {
		igraph_vector_destroy(top->edgeWeights);
		g_free(top->edgeWeights);
	}
	top->edgeWeights = NULL;

	igraph_destroy(&top->graph);

	g_mutex_unlock(&top->graphLock);
	g_mutex_clear(&(top->graphLock));

	MAGIC_CLEAR(top);
	g_free(top);
}

Topology* topology_new(gchar* graphPath) {
	utility_assert(graphPath);
	Topology* top = g_new0(Topology, 1);
	MAGIC_INIT(top);

	top->graphPath = g_string_new(graphPath);
	top->virtualIP = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);

	g_mutex_init(&(top->graphLock));
	g_rw_lock_init(&(top->virtualIPLock));
	g_rw_lock_init(&(top->pathCacheLock));

	/* first read in the graph and make sure its formed correctly,
	 * then setup our edge weights for shortest path */
	if(!_topology_loadGraph(top) || !_topology_checkGraph(top) ||
			!_topology_extractEdgeWeights(top)) {
		topology_free(top);
		return NULL;
	}

	return top;
}
