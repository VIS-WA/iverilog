/*
 * Copyright (c) 2007-2008 Stephen Williams (steve@icarus.com)
 *
 *    This source code is free software; you can redistribute it
 *    and/or modify it in source code form under the terms of the GNU
 *    General Public License as published by the Free Software
 *    Foundation; either version 2 of the License, or (at your option)
 *    any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

# include  "array.h"
#include  "symbols.h"
#include  "schedule.h"
#include  "vpi_priv.h"
#ifdef HAVE_MALLOC_H
# include  <malloc.h>
#endif
# include  <stdlib.h>
# include  <string.h>
# include  <iostream>
# include  "compile.h"
# include  <assert.h>

unsigned long count_net_arrays = 0;
unsigned long count_net_array_words = 0;
unsigned long count_var_arrays = 0;
unsigned long count_var_array_words = 0;
unsigned long count_real_arrays = 0;
unsigned long count_real_array_words = 0;

static symbol_map_s<struct __vpiArray>* array_table =0;

class vvp_fun_arrayport;
static void array_attach_port(vvp_array_t, vvp_fun_arrayport*);

vvp_array_t array_find(const char*label)
{
      if (array_table == 0)
	    return 0;

      vvp_array_t v = array_table->sym_get_value(label);
      return v;
}

/*
* The vpiArray object holds an array of vpi objects that themselves
* represent the words of the array. The vpi_array_t is a pointer to this.
*/
struct __vpiArray {
      __vpiArray() { }

      struct __vpiHandle base;
      struct __vpiScope*scope;
      const char*name; /* Permanently allocated string */
      unsigned array_count;
      struct __vpiDecConst first_addr;
      struct __vpiDecConst last_addr;
      struct __vpiDecConst msb;
      struct __vpiDecConst lsb;
      unsigned vals_width;
	// If this is a net array, nets lists the handles.
      vpiHandle*nets;
	// If this is a var array, then these are used instead of nets.
      vvp_vector4array_t   *vals;
      struct __vpiArrayWord*vals_words;

      class vvp_fun_arrayport*ports_;
      struct __vpiCallback *vpi_callbacks;
      bool signed_flag;
};

struct __vpiArrayIterator {
      struct __vpiHandle base;
      struct __vpiArray*array;
      unsigned next;
};

struct __vpiArrayIndex {
      struct __vpiHandle base;
      struct __vpiDecConst *index;
      unsigned done;
};

struct __vpiArrayVthrA {
      struct __vpiHandle base;
      struct __vpiArray*array;
	// If this is set, then use it to get the index value.
      vpiHandle address_handle;
	// If wid==0, then address is the address into the array.
      unsigned address;
	// If wid >0, then the address is the base and wid the vector
	// width of the index to pull from the thread.
      unsigned wid;

      unsigned get_address() const
      {
	    if (address_handle) {
		  s_vpi_value vp;
		  vp.format = vpiIntVal;
		  vpi_get_value(address_handle, &vp);
		  return vp.value.integer;
	    }

	    if (wid == 0)
		  return address;

	    vvp_vector4_t tmp (wid);
	    for (unsigned idx = 0 ; idx < wid ; idx += 1) {
		  vvp_bit4_t bit = vthread_get_bit(vpip_current_vthread, address+idx);
		  tmp.set_bit(idx, bit);
	    }
	    unsigned long val;
	    vector4_to_value(tmp, val);
	    return val;
      }
};

/* Get the array word size. This has only been checked for reg arrays. */
unsigned get_array_word_size(vvp_array_t array)
{
      assert(array->vals);
      return  array->vals_width;
}

/*
 * The vpiArrayWord is magic. It is used as the handle to return when
 * vpi code tries to index or scan an array of variable words. The
 * array word handle contains no actual data. It is just a hook for
 * the vpi methods and to point to the parent.
 *
 * How the point to the parent works is tricky. The vpiArrayWord
 * objects for an array are themselves allocated as an array. All the
 * ArrayWord objects in the array have a word0 that points to the base
 * of the array. Thus, the position into the array (and the index into
 * the memory) is calculated by subtracting word0 from the ArrayWord
 * pointer.
 *
 * To then get to the parent, use word0[-1].parent.
 *
 * The vpiArrayWord is also used as a handle for the index (vpiIndex)
 * for the word. To make that work, return the pointer to the as_index
 * member instead of the as_word member. The result is a different set
 * of vpi functions is bound to the same structure. All the details
 * for the word also apply when treating this as an index.
 */
struct __vpiArrayWord {
      struct __vpiHandle as_word;
      struct __vpiHandle as_index;
      union {
	    struct __vpiArray*parent;
	    struct __vpiArrayWord*word0;
      };
};


static int vpi_array_get(int code, vpiHandle ref);
static char*vpi_array_get_str(int code, vpiHandle ref);
static vpiHandle vpi_array_get_handle(int code, vpiHandle ref);
static vpiHandle vpi_array_iterate(int code, vpiHandle ref);
static vpiHandle vpi_array_index(vpiHandle ref, int index);

static vpiHandle array_iterator_scan(vpiHandle ref, int);
static int array_iterator_free_object(vpiHandle ref);

static vpiHandle array_index_scan(vpiHandle ref, int);
static int array_index_free_object(vpiHandle ref);

static int vpi_array_var_word_get(int code, vpiHandle);
static char*vpi_array_var_word_get_str(int code, vpiHandle);
static void vpi_array_var_word_get_value(vpiHandle, p_vpi_value);
static vpiHandle vpi_array_var_word_put_value(vpiHandle, p_vpi_value, int);
static vpiHandle vpi_array_var_word_get_handle(int code, vpiHandle ref);

static void vpi_array_var_index_get_value(vpiHandle, p_vpi_value);

static int vpi_array_vthr_A_get(int code, vpiHandle);
static char*vpi_array_vthr_A_get_str(int code, vpiHandle);
static void vpi_array_vthr_A_get_value(vpiHandle, p_vpi_value);
static vpiHandle vpi_array_vthr_A_put_value(vpiHandle, p_vpi_value, int);
static vpiHandle vpi_array_vthr_A_get_handle(int code, vpiHandle ref);

static const struct __vpirt vpip_arraymem_rt = {
      vpiMemory,
      vpi_array_get,
      vpi_array_get_str,
      0,
      0,
      vpi_array_get_handle,
      vpi_array_iterate,
      vpi_array_index,
};

static const struct __vpirt vpip_array_iterator_rt = {
      vpiIterator,
      0,
      0,
      0,
      0,
      0,
      0,
      array_iterator_scan,
      &array_iterator_free_object
};

/* This should look a bit odd since it provides a fake iteration on
 * this object. This trickery is used to implement the two forms of
 * index access, simple handle access and iteration access. */
static const struct __vpirt vpip_array_index_rt = {
      vpiIterator,
      0,
      0,
      0,
      0,
      0,
      array_index_iterate,
      array_index_scan,
      array_index_free_object
};

static const struct __vpirt vpip_array_var_word_rt = {
      vpiMemoryWord,
      &vpi_array_var_word_get,
      &vpi_array_var_word_get_str,
      &vpi_array_var_word_get_value,
      &vpi_array_var_word_put_value,
      &vpi_array_var_word_get_handle,
      0,
      0,
      0
};

static const struct __vpirt vpip_array_var_index_rt = {
      vpiIndex,
      0,
      0,
      &vpi_array_var_index_get_value,
      0,
      0,
      0,
      0,
      0
};

static const struct __vpirt vpip_array_vthr_A_rt = {
      vpiMemoryWord,
      &vpi_array_vthr_A_get,
      &vpi_array_vthr_A_get_str,
      &vpi_array_vthr_A_get_value,
      &vpi_array_vthr_A_put_value,
      &vpi_array_vthr_A_get_handle,
      0,
      0,
      0
};

# define ARRAY_HANDLE(ref) (assert(ref->vpi_type->type_code==vpiMemory), \
			    (struct __vpiArray*)ref)

static struct __vpiArrayWord* array_var_word_from_handle(vpiHandle ref)
{
      if (ref == 0)
	    return 0;
      if (ref->vpi_type != &vpip_array_var_word_rt)
	    return 0;

      return (struct __vpiArrayWord*) ref;
}

static struct __vpiArrayWord* array_var_index_from_handle(vpiHandle ref)
{
      if (ref == 0)
	    return 0;
      if (ref->vpi_type != &vpip_array_var_index_rt)
	    return 0;

      return (struct __vpiArrayWord*) (ref-1);
}

static struct __vpiArrayVthrA* array_vthr_a_from_handle(vpiHandle ref)
{
      if (ref == 0)
	    return 0;
      if (ref->vpi_type != &vpip_array_vthr_A_rt)
	    return 0;

      return (struct __vpiArrayVthrA*) ref;
}

static void array_make_vals_words(struct __vpiArray*parent)
{
      assert(parent->vals_words == 0);
      parent->vals_words = new struct __vpiArrayWord[parent->array_count + 1];

	// Make word[-1] point to the parent.
      parent->vals_words->parent = parent;
	// Now point to word-0
      parent->vals_words += 1;

      struct __vpiArrayWord*words = parent->vals_words;
      for (unsigned idx = 0 ; idx < parent->array_count ; idx += 1) {
	    words[idx].as_word.vpi_type = &vpip_array_var_word_rt;
	    words[idx].as_index.vpi_type = &vpip_array_var_index_rt;
	    words[idx].word0 = words;
      }
}

static unsigned decode_array_word_pointer(struct __vpiArrayWord*word,
					  struct __vpiArray*&parent)
{
      struct __vpiArrayWord*word0 = word->word0;
      parent = (word0 - 1) -> parent;
      return word - word0;
}

static int vpi_array_get(int code, vpiHandle ref)
{
      struct __vpiArray*obj = ARRAY_HANDLE(ref);

      switch (code) {
	  case vpiLineNo:
	    return 0; // Not implemented for now!

	  case vpiSize:
	    return (int) obj->array_count;

	  default:
	    return 0;
      }
}

static char*vpi_array_get_str(int code, vpiHandle ref)
{
      struct __vpiArray*obj = ARRAY_HANDLE(ref);

      if (code == vpiFile) {  // Not implemented for now!
            return simple_set_rbuf_str(file_names[0]);
      }

      return generic_get_str(code, &obj->scope->base, obj->name, NULL);
}

static vpiHandle vpi_array_get_handle(int code, vpiHandle ref)
{
      struct __vpiArray*obj = ARRAY_HANDLE(ref);

      switch (code) {

	  case vpiLeftRange:
	    return &(obj->first_addr.base);

	  case vpiRightRange:
	    return &(obj->last_addr.base);

	  case vpiScope:
	    return &obj->scope->base;
      }

      return 0;
}

static vpiHandle vpi_array_iterate(int code, vpiHandle ref)
{
      struct __vpiArray*obj = ARRAY_HANDLE(ref);

      switch (code) {

	  case vpiMemoryWord: {
		struct __vpiArrayIterator*res;
		res = (struct __vpiArrayIterator*) calloc(1, sizeof (*res));
		res->base.vpi_type = &vpip_array_iterator_rt;
		res->array = obj;
		res->next = 0;
		return &res->base;
	  }

      }

      return 0;
}

/*
* VPI code passes indices that are not yet converted to canonical
* form, so this index function does it here.
*/
static vpiHandle vpi_array_index(vpiHandle ref, int index)
{
      struct __vpiArray*obj = ARRAY_HANDLE(ref);

      index -= obj->first_addr.value;
      if (index >= (long)obj->array_count)
	    return 0;
      if (index < 0)
	    return 0;

      if (obj->nets != 0) {
	    return obj->nets[index];
      }

      if (obj->vals_words == 0)
	    array_make_vals_words(obj);

      return &(obj->vals_words[index].as_word);
}

static int vpi_array_var_word_get(int code, vpiHandle ref)
{
      struct __vpiArrayWord*obj = array_var_word_from_handle(ref);
      struct __vpiArray*parent;

      assert(obj);
      decode_array_word_pointer(obj, parent);

      switch (code) {
	  case vpiSize:
	    return (int) parent->vals->width();

	  case vpiLeftRange:
	    return parent->msb.value;

	  case vpiRightRange:
	    return parent->lsb.value;

	  default:
	    return 0;
      }
}

static char*vpi_array_var_word_get_str(int code, vpiHandle ref)
{
      struct __vpiArrayWord*obj = array_var_word_from_handle(ref);
      struct __vpiArray*parent;

      assert(obj);
      unsigned index = decode_array_word_pointer(obj, parent);

      if (code == vpiFile) {  // Not implemented for now!
	    return simple_set_rbuf_str(file_names[0]);
      }

      char sidx [64];
      snprintf(sidx, 63, "%d", (int)index + parent->first_addr.value);
      return generic_get_str(code, &parent->scope->base, parent->name, sidx);
}

static void vpi_array_var_word_get_value(vpiHandle ref, p_vpi_value value)
{
      struct __vpiArrayWord*obj = array_var_word_from_handle(ref);
      struct __vpiArray*parent;

      assert(obj);
      unsigned index = decode_array_word_pointer(obj, parent);
      unsigned width = parent->vals->width();

      vpip_vec4_get_value(parent->vals->get_word(index), width,
                          parent->signed_flag, value);
}

static vpiHandle vpi_array_var_word_put_value(vpiHandle ref, p_vpi_value vp, int flags)
{
      struct __vpiArrayWord*obj = array_var_word_from_handle(ref);
      struct __vpiArray*parent;

      assert(obj);
      unsigned index = decode_array_word_pointer(obj, parent);

      vvp_vector4_t val = vec4_from_vpi_value(vp, parent->vals_width);
      array_set_word(parent, index, 0, val);
      return ref;
}

static vpiHandle vpi_array_var_word_get_handle(int code, vpiHandle ref)
{
      struct __vpiArrayWord*obj = array_var_word_from_handle(ref);
      struct __vpiArray*parent;

      assert(obj);
      decode_array_word_pointer(obj, parent);

      switch (code) {

	  case vpiIndex:
	    return &(obj->as_index);

	  case vpiLeftRange:
	    return &parent->msb.base;

	  case vpiRightRange:
	    return &parent->lsb.base;

	  case vpiParent:
	    return &parent->base;

	  case vpiScope:
	    return &parent->scope->base;
      }

      return 0;
}

static void vpi_array_var_index_get_value(vpiHandle ref, p_vpi_value value)
{
      struct __vpiArrayWord*obj = array_var_index_from_handle(ref);
      struct __vpiArray*parent;

      assert(obj);
      unsigned index = decode_array_word_pointer(obj, parent);

      assert(value->format == vpiIntVal);
      value->value.integer = index;
}

# define ARRAY_ITERATOR(ref) (assert(ref->vpi_type->type_code==vpiIterator), \
			      (struct __vpiArrayIterator*)ref)

static vpiHandle array_iterator_scan(vpiHandle ref, int)
{
      struct __vpiArrayIterator*obj = ARRAY_ITERATOR(ref);

      if (obj->next >= obj->array->array_count) {
	    vpi_free_object(ref);
	    return 0;
      }

      unsigned use_index = obj->next;
      obj->next += 1;

      if (obj->array->nets)
	    return obj->array->nets[obj->next];

      assert(obj->array->vals);

      if (obj->array->vals_words == 0)
	    array_make_vals_words(obj->array);

      return &(obj->array->vals_words[use_index].as_word);
}

static int array_iterator_free_object(vpiHandle ref)
{
      struct __vpiArrayIterator*obj = ARRAY_ITERATOR(ref);
      free(obj);
      return 1;
}

# define ARRAY_INDEX(ref) (assert(ref->vpi_type->type_code==vpiIterator), \
			   (struct __vpiArrayIndex*)ref)

vpiHandle array_index_iterate(int code, vpiHandle ref)
{
      assert(ref->vpi_type->type_code == vpiConstant);
      struct __vpiDecConst*obj = (struct __vpiDecConst*)ref;

      if (code == vpiIndex) {
	    struct __vpiArrayIndex*res;
	    res = (struct __vpiArrayIndex*) calloc(1, sizeof (*res));
	    res->base.vpi_type = &vpip_array_index_rt;
	    res->index = obj;
	    res->done = 0;
	    return &res->base;
      }
      return 0;
}

static vpiHandle array_index_scan(vpiHandle ref, int)
{
      struct __vpiArrayIndex*obj = ARRAY_INDEX(ref);

      if (obj->done == 0) {
            obj->done = 1;
            return &obj->index->base;
      }

      vpi_free_object(ref);
      return 0;
}

static int array_index_free_object(vpiHandle ref)
{
      struct __vpiArrayIndex*obj = ARRAY_INDEX(ref);
      free(obj);
      return 1;
}

static int vpi_array_vthr_A_get(int code, vpiHandle ref)
{
      struct __vpiArrayVthrA*obj = array_vthr_a_from_handle(ref);
      assert(obj);
      struct __vpiArray*parent = obj->array;

      switch (code) {
	  case vpiLineNo:
	    return 0; // Not implemented for now!

	  case vpiSize:
	    return parent->vals_width;

	  case vpiLeftRange:
	    return parent->msb.value;

	  case vpiRightRange:
	    return parent->lsb.value;

	  // For now &A<> is only a constant select. This will need
	  // to be changed when it supports variable selection.
	  case vpiConstantSelect:
	    return 1;

	  default:
	    return 0;
      }
}

static char*vpi_array_vthr_A_get_str(int code, vpiHandle ref)
{
      struct __vpiArrayVthrA*obj = array_vthr_a_from_handle(ref);
      assert(obj);
      struct __vpiArray*parent = obj->array;

      if (code == vpiFile) {  // Not implemented for now!
            return simple_set_rbuf_str(file_names[0]);
      }

      char sidx [64];
      snprintf(sidx, 63, "%d", (int)obj->get_address() + parent->first_addr.value);
      return generic_get_str(code, &parent->scope->base, parent->name, sidx);
}

static void vpi_array_vthr_A_get_value(vpiHandle ref, p_vpi_value value)
{
      struct __vpiArrayVthrA*obj = array_vthr_a_from_handle(ref);
      assert(obj);
      struct __vpiArray*parent = obj->array;

      assert(parent);

      unsigned index = obj->get_address();
      vvp_vector4_t tmp = array_get_word(parent, index);
      vpip_vec4_get_value(tmp, parent->vals_width, parent->signed_flag, value);
}

static vpiHandle vpi_array_vthr_A_put_value(vpiHandle ref, p_vpi_value vp, int)
{
      struct __vpiArrayVthrA*obj = array_vthr_a_from_handle(ref);
      assert(obj);
      struct __vpiArray*parent = obj->array;

      unsigned index = obj->get_address();

      assert(parent);
      assert(index < parent->array_count);

      vvp_vector4_t val = vec4_from_vpi_value(vp, parent->vals_width);
      array_set_word(parent, index, 0, val);

      return ref;
}

static vpiHandle vpi_array_vthr_A_get_handle(int code, vpiHandle ref)
{
      struct __vpiArrayVthrA*obj = array_vthr_a_from_handle(ref);
      assert(obj);
      struct __vpiArray*parent = obj->array;

      switch (code) {

	  case vpiIndex:
	    break;  // Not implemented!

	  case vpiLeftRange:
	    return &parent->msb.base;

	  case vpiRightRange:
	    return &parent->lsb.base;

	  case vpiParent:
	    return &parent->base;

	  case vpiScope:
	    return &parent->scope->base;
      }

      return 0;
}

void array_set_word(vvp_array_t arr,
		    unsigned address,
		    unsigned part_off,
		    vvp_vector4_t val)
{
      if (address >= arr->array_count)
	    return;

      if (arr->vals) {
	    assert(arr->nets == 0);
	    if (part_off != 0 || val.size() != arr->vals_width) {
		  vvp_vector4_t tmp = arr->vals->get_word(address);
		  if ((part_off + val.size()) > tmp.size()) {
			cerr << "part_off=" << part_off
			     << " val.size()=" << val.size()
			     << " arr->vals[address].size()=" << tmp.size()
			     << " arr->vals_width=" << arr->vals_width << endl;
			assert(0);
		  }
		  tmp.set_vec(part_off, val);
		  arr->vals->set_word(address, tmp);
	    } else {
		  arr->vals->set_word(address, val);
	    }
	    array_word_change(arr, address);
	    return;
      }

      assert(arr->nets != 0);

	// Select the word of the array that we affect.
      vpiHandle word = arr->nets[address];
      struct __vpiSignal*vsig = vpip_signal_from_handle(word);
      assert(vsig);

      vvp_net_ptr_t ptr (vsig->node, 0);
      vvp_send_vec4_pv(ptr, val, part_off, val.size(), vpip_size(vsig));
      array_word_change(arr, address);
}

vvp_vector4_t array_get_word(vvp_array_t arr, unsigned address)
{
      if (arr->vals) {
	    assert(arr->nets == 0);

	    return arr->vals->get_word(address);
      }

      assert(arr->vals == 0);
      assert(arr->nets != 0);

      if (address >= arr->array_count) {
	      // Reading outside the array. Return X's but get the
	      // width by looking at a word that we know is present.
	    assert(arr->array_count > 0);
	    vpiHandle word = arr->nets[0];
	    struct __vpiSignal*vsig = vpip_signal_from_handle(word);
	    assert(vsig);
	    vvp_fun_signal_vec*sig = dynamic_cast<vvp_fun_signal_vec*> (vsig->node->fun);
	    assert(sig);
	    return vvp_vector4_t(sig->size(), BIT4_X);
      }

      vpiHandle word = arr->nets[address];
      struct __vpiSignal*vsig = vpip_signal_from_handle(word);
      assert(vsig);
      vvp_fun_signal_vec*sig = dynamic_cast<vvp_fun_signal_vec*> (vsig->node->fun);
      assert(sig);

      vvp_vector4_t val = sig->vec4_value();
      return val;
}

static vpiHandle vpip_make_array(char*label, const char*name,
				 int first_addr, int last_addr,
				 bool signed_flag)
{
      struct __vpiArray*obj = (struct __vpiArray*)
	    malloc(sizeof(struct __vpiArray));

      obj->signed_flag = signed_flag;

	// Assume increasing addresses.
      assert(last_addr >= first_addr);
      unsigned array_count = last_addr+1-first_addr;

	// For now, treat all arrays as memories. This is not quite
	// correct, as arrays are arrays with memories a special case.
      obj->base.vpi_type = &vpip_arraymem_rt;
      obj->scope = vpip_peek_current_scope();
      obj->name  = vpip_name_string(name);
      obj->array_count = array_count;

      vpip_make_dec_const(&obj->first_addr, first_addr);
      vpip_make_dec_const(&obj->last_addr, last_addr);

	// Start off now knowing if we are nets or variables.
      obj->nets = 0;
      obj->vals = 0;
      obj->vals_width = 0;
      vpip_make_dec_const(&obj->msb, 0);
      vpip_make_dec_const(&obj->lsb, 0);
      obj->vals_words = 0;

	// Initialize (clear) the read-ports list.
      obj->ports_ = 0;
      obj->vpi_callbacks = 0;

	/* Add this symbol to the array_symbols table for later lookup. */
      if (!array_table)
	    array_table = new symbol_map_s<struct __vpiArray>;

      assert(!array_find(label));
      array_table->sym_set_value(label, obj);

	/* Add this into the table of VPI objects. This is used for
	   contexts that try to look up VPI objects in
	   general. (i.e. arguments to vpi_task calls.) */
      compile_vpi_symbol(label, &(obj->base));

	/* Blindly attach to the scope as an object. */
      vpip_attach_to_current_scope(&(obj->base));

      return &(obj->base);
}

void array_alias_word(vvp_array_t array, unsigned long addr, vpiHandle word)
{
      assert(addr < array->array_count);
      assert(array->nets);
      array->nets[addr] = word;
}

void array_attach_word(vvp_array_t array, unsigned long addr, vpiHandle word)
{
      assert(addr < array->array_count);
      assert(array->nets);
      array->nets[addr] = word;

      if (struct __vpiSignal*sig = vpip_signal_from_handle(word)) {
	    vvp_net_t*net = sig->node;
	    assert(net);
	    vvp_fun_signal_base*fun = dynamic_cast<vvp_fun_signal_base*>(net->fun);
	    assert(fun);
	    fun->attach_as_word(array, addr);
	    sig->is_netarray = 1;
	    sig->within.parent = &array->base;
	    sig->id.index = vpip_make_dec_const(addr + array->first_addr.value);
      }
}

void compile_var_array(char*label, char*name, int last, int first,
		   int msb, int lsb, char signed_flag)
{
      vpiHandle obj = vpip_make_array(label, name, first, last,
                                      signed_flag != 0);

      struct __vpiArray*arr = ARRAY_HANDLE(obj);

	/* Make the words. */
      arr->vals_width = labs(msb-lsb) + 1;
      arr->vals = new vvp_vector4array_t(arr->vals_width, arr->array_count,
                                         vpip_peek_current_scope()->is_automatic);
      vpip_add_item_to_current_scope(arr->vals);
      vpip_make_dec_const(&arr->msb, msb);
      vpip_make_dec_const(&arr->lsb, lsb);

      count_var_arrays += 1;
      count_var_array_words += arr->array_count;

      free(label);
      free(name);
}

void compile_real_array(char*label, char*name, int last, int first,
			int msb, int lsb)
{
      vpiHandle obj = vpip_make_array(label, name, first, last, true);

      struct __vpiArray*arr = ARRAY_HANDLE(obj);
      vvp_array_t array = array_find(label);

	/* Make the words. */
      for (unsigned idx = 0 ;  idx < arr->array_count ;  idx += 1) {
	    char buf[64];
	    snprintf(buf, sizeof buf, "%s_%u", label, idx);
	    compile_varw_real(strdup(buf), array, idx, msb, lsb);
      }

      count_real_arrays += 1;
      count_real_array_words += arr->array_count;

      free(label);
      free(name);
}

void compile_net_array(char*label, char*name, int last, int first)
{
      vpiHandle obj = vpip_make_array(label, name, first, last, false);

      struct __vpiArray*arr = ARRAY_HANDLE(obj);
      arr->nets = (vpiHandle*)calloc(arr->array_count, sizeof(vpiHandle));

      count_net_arrays += 1;
      count_net_array_words += arr->array_count;

      free(label);
      free(name);
}

class vvp_fun_arrayport  : public vvp_net_fun_t {

    public:
      explicit vvp_fun_arrayport(vvp_array_t mem, vvp_net_t*net);
      explicit vvp_fun_arrayport(vvp_array_t mem, vvp_net_t*net, long addr);
      ~vvp_fun_arrayport();

      void check_word_change(unsigned long addr);

      void recv_vec4(vvp_net_ptr_t port, const vvp_vector4_t&bit);

    private:
      vvp_array_t arr_;
      vvp_net_t  *net_;
      unsigned long addr_;

      friend void array_attach_port(vvp_array_t, vvp_fun_arrayport*);
      friend void array_word_change(vvp_array_t, unsigned long);
      vvp_fun_arrayport*next_;
};

vvp_fun_arrayport::vvp_fun_arrayport(vvp_array_t mem, vvp_net_t*net)
: arr_(mem), net_(net), addr_(0)
{
      next_ = 0;
}

vvp_fun_arrayport::vvp_fun_arrayport(vvp_array_t mem, vvp_net_t*net, long addr)
: arr_(mem), net_(net), addr_(addr)
{
      next_ = 0;
}

vvp_fun_arrayport::~vvp_fun_arrayport()
{
}

void vvp_fun_arrayport::recv_vec4(vvp_net_ptr_t port, const vvp_vector4_t&bit)
{
      bool addr_valid_flag;

      switch (port.port()) {

	  case 0: // Address input
	    addr_valid_flag = vector4_to_value(bit, addr_);
	    if (! addr_valid_flag)
		  addr_ = arr_->array_count;
	    vvp_send_vec4(port.ptr()->out, array_get_word(arr_,addr_));
	    break;

	  default:
	    fprintf(stdout, "XXXX write ports not implemented.\n");
	    assert(0);
      }
}

void vvp_fun_arrayport::check_word_change(unsigned long addr)
{
      if (addr != addr_)
	    return;

      vvp_vector4_t bit = array_get_word(arr_, addr_);
      vvp_send_vec4(net_->out, bit);
}

static void array_attach_port(vvp_array_t array, vvp_fun_arrayport*fun)
{
      assert(fun->next_ == 0);
      fun->next_ = array->ports_;
      array->ports_ = fun;
}

void array_word_change(vvp_array_t array, unsigned long addr)
{
      for (vvp_fun_arrayport*cur = array->ports_; cur; cur = cur->next_)
	    cur->check_word_change(addr);

	// Run callbacks attached to the array itself.
      struct __vpiCallback *next = array->vpi_callbacks;
      struct __vpiCallback *prev = 0;

      while (next) {
	    struct __vpiCallback*cur = next;
	    next = cur->next;

	      // Skip callbacks that are not for me. -1 is for every element.
	    if (cur->extra_data != (long)addr && cur->extra_data != -1) {
		  prev = cur;
		  continue;
	    }

	      // For whole array callbacks we need to set the index.
	    if (cur->extra_data == -1) {
		  cur->cb_data.index = (PLI_INT32) ((int)addr +
		                       array->first_addr.value);
	    }

	    if (cur->cb_data.cb_rtn != 0) {
		  if (cur->cb_data.value)
			vpip_vec4_get_value(array->vals->get_word(addr),
					    array->vals_width,
					    array->signed_flag,
					    cur->cb_data.value);

		  callback_execute(cur);
		  prev = cur;

	    } else if (prev == 0) {

		  array->vpi_callbacks = next;
		  cur->next = 0;
		  delete_vpi_callback(cur);

	    } else {
		  assert(prev->next == cur);
		  prev->next = next;
		  cur->next = 0;
		  delete_vpi_callback(cur);
	    }
      }
}

class array_port_resolv_list_t : public resolv_list_s {

    public:
      explicit array_port_resolv_list_t(char*lab) : resolv_list_s(lab) { }

      vvp_net_t*ptr;
      bool use_addr;
      long addr;
      bool resolve(bool mes);

    private:
};

bool array_port_resolv_list_t::resolve(bool mes)
{
      vvp_array_t mem = array_find(label());
      if (mem == 0) {
	    assert(mem || !mes);
	    return false;
      }

      vvp_fun_arrayport*fun;
      if (use_addr)
	    fun = new vvp_fun_arrayport(mem, ptr, addr);
      else
	    fun = new vvp_fun_arrayport(mem, ptr);
      ptr->fun = fun;

      array_attach_port(mem, fun);

      return true;
}

void vpip_array_word_change(struct __vpiCallback*cb, vpiHandle obj)
{
      struct __vpiArray*parent = 0;
      if (struct __vpiArrayWord*word = array_var_word_from_handle(obj)) {
	    unsigned addr = decode_array_word_pointer(word, parent);
	    cb->extra_data = addr;

      } else if (struct __vpiArrayVthrA*tword = array_vthr_a_from_handle(obj)) {
	    parent = tword->array;
	    cb->extra_data = tword->address;
      }

      assert(parent);
      cb->next = parent->vpi_callbacks;
      parent->vpi_callbacks = cb;
}

void vpip_array_change(struct __vpiCallback*cb, vpiHandle obj)
{
      
      struct __vpiArray*arr = ARRAY_HANDLE(obj);
      cb->extra_data = -1; // This is a callback for every element.
      cb->next = arr->vpi_callbacks;
      arr->vpi_callbacks = cb;
}

void compile_array_port(char*label, char*array, char*addr)
{
      array_port_resolv_list_t*resolv_mem
	    = new array_port_resolv_list_t(array);

      resolv_mem->ptr = new vvp_net_t;
      resolv_mem->use_addr = false;
      define_functor_symbol(label, resolv_mem->ptr);
      free(label);
	// Connect the port-0 input as the address.
      input_connect(resolv_mem->ptr, 0, addr);

      resolv_submit(resolv_mem);
}

void compile_array_port(char*label, char*array, long addr)
{
      array_port_resolv_list_t*resolv_mem
	    = new array_port_resolv_list_t(array);

      resolv_mem->ptr = new vvp_net_t;
      resolv_mem->use_addr = true;
      resolv_mem->addr = addr;
      define_functor_symbol(label, resolv_mem->ptr);
      free(label);

      resolv_submit(resolv_mem);
}

void compile_array_alias(char*label, char*name, char*src)
{
      vvp_array_t mem = array_find(src);
      assert(mem);

      struct __vpiArray*obj = (struct __vpiArray*)
	    malloc(sizeof (struct __vpiArray));

      obj->base.vpi_type = &vpip_arraymem_rt;
      obj->scope = vpip_peek_current_scope();
      obj->name  = vpip_name_string(name);
      obj->array_count = mem->array_count;

	// XXXX Need to set an accurate range of addresses.
      vpip_make_dec_const(&obj->first_addr, mem->first_addr.value);
      vpip_make_dec_const(&obj->last_addr, mem->last_addr.value);

	// Share the words with the source array.
      obj->nets = mem->nets;
      obj->vals = mem->vals;

      obj->ports_ = 0;

      assert(array_table);
      assert(!array_find(label));
      array_table->sym_set_value(label, obj);

      compile_vpi_symbol(label, &obj->base);
      vpip_attach_to_current_scope(&obj->base);

      free(label);
      free(name);
      free(src);
}

vpiHandle vpip_make_vthr_A(char*label, unsigned addr)
{
      struct __vpiArrayVthrA*obj = (struct __vpiArrayVthrA*)
	    malloc(sizeof (struct __vpiArrayVthrA));

      obj->base.vpi_type = &vpip_array_vthr_A_rt;

      obj->array = array_find(label);
      assert(obj->array);
      free(label);

      obj->address_handle = 0;
      obj->address = addr;
      obj->wid = 0;
      assert(addr < obj->array->array_count);

      return &(obj->base);
}

vpiHandle vpip_make_vthr_A(char*label, unsigned tbase, unsigned twid)
{
      struct __vpiArrayVthrA*obj = (struct __vpiArrayVthrA*)
	    malloc(sizeof (struct __vpiArrayVthrA));

      obj->base.vpi_type = &vpip_array_vthr_A_rt;

      obj->array = array_find(label);
      assert(obj->array);
      free(label);

      obj->address_handle = 0;
      obj->address = tbase;
      obj->wid     = twid;

      return &(obj->base);
}

vpiHandle vpip_make_vthr_A(char*label, char*symbol)
{
      struct __vpiArrayVthrA*obj = (struct __vpiArrayVthrA*)
	    malloc(sizeof (struct __vpiArrayVthrA));

      obj->base.vpi_type = &vpip_array_vthr_A_rt;

      obj->array = array_find(label);
      assert(obj->array);
      free(label);

      obj->address_handle = 0;
      compile_vpi_lookup(&obj->address_handle, symbol);
      obj->address = 0;
      obj->wid = 0;

      return &(obj->base);
}
vpiHandle vpip_make_vthr_A(char*label, vpiHandle handle)
{
      struct __vpiArrayVthrA*obj = (struct __vpiArrayVthrA*)
	    malloc(sizeof (struct __vpiArrayVthrA));

      obj->base.vpi_type = &vpip_array_vthr_A_rt;

      obj->array = array_find(label);
      assert(obj->array);
      free(label);

      obj->address_handle = handle;
      obj->address = 0;
      obj->wid = 0;

      return &(obj->base);
}

void compile_array_cleanup(void)
{
      if (array_table) {
	    delete array_table;
	    array_table = 0;
      }
}
